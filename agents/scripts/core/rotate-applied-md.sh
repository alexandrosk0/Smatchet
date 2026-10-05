#!/usr/bin/env bash
# agents/scripts/core/rotate-applied-md.sh — bound applied.md by rotating old
# months into flat sibling partitions.
#
# applied.md is a monotonically growing append-target (archive-backlog-entry.sh
# prepends; sort-applied-md.sh keeps "Latest first"). Unbounded, it passed 1 MB
# within ~3.5 months. This script moves every entry OLDER than the current and
# previous calendar month into `applied-YYYY-MM.md` next to applied.md — flat
# siblings, NOT a subdirectory, deliberately: archive-backlog-entry.sh rewrites
# each archived entry's relative links to applied.md's exact depth
# (`categories/`), so a deeper partition dir would break every link in a
# rotated entry. Same-depth siblings keep them valid with zero rewriting.
#
# Entries come in two shapes — legacy `- YYYY-MM-DD · …` list blocks and
# per-entry-file blocks (`# <title>` + a `**Date**:`-style metadata paragraph)
# — split by the shared applied_md_lib.py. Every block rotates by its OWN date;
# the legacy-only splitter glued a per-entry block onto the dated block above it
# and filed it under that block's month. A partition block whose own date names
# another month is re-homed (to its own partition, or back to the head when that
# month is still current) on the next run.
#
# Partition files are created with a standard header (including the
# deleted-runtime banner, which is self-scoping: it annotates only entries
# that reference the removed agentic-flow C++ runtime) and are sorted latest
# first, like the head. Rotation appends to an existing partition and re-sorts
# it, dropping only exact copies of blocks the partition already holds.
# Idempotent: a second run is a no-op.
#
# Invoked automatically by archive-backlog-entry.sh after each append, so the
# head stays bounded by construction. test-backlog-counts.sh runs `--check`
# as an ADVISORY (WARN-only) freshness signal — a month boundary can make
# rotation "due" with no accompanying change, so a hard gate would spontaneously
# red CI; the next archival rotates for real.
#
# Usage:
#   bash agents/scripts/core/rotate-applied-md.sh            # rotate in place
#   bash agents/scripts/core/rotate-applied-md.sh --check    # exit 1 if rotation is due
#
# Env: ROTATE_APPLIED_TODAY=YYYY-MM-DD  pin "today" (fixtures; default: the date).
#
# Exit codes:
#   0 — rotated (or nothing to rotate; or --check with nothing due)
#   1 — --check mode and rotation is due; OR Python error via `set -e`
#   2 — applied.md not found / no python

set -euo pipefail

ROTATE_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$ROTATE_LIB_DIR/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "python3 required (no working interpreter on PATH)" >&2; exit 2; }

# applied.md is HOST content — self-improvement entries never move into the agent
# layer — so resolve it from the caller's tree (the git toplevel of cwd), not from
# this script's location: once the layer is a submodule that location is the layer
# root, and a fixture repo that runs this script must rotate ITS applied.md, never
# the real one. Outside any git work tree, fall back to the script-relative root.
_rot_root="$(git rev-parse --show-toplevel 2>/dev/null)" || _rot_root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$_rot_root" || { echo "rotate-applied-md: cannot cd to $_rot_root" >&2; exit 2; }

APPLIED="docs/self-improvement/categories/applied.md"
CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
fi

if [ ! -f "$APPLIED" ]; then
    echo "rotate-applied-md: $APPLIED not found" >&2
    exit 2
fi

"$PY" - "$APPLIED" "$CHECK_ONLY" "$ROTATE_LIB_DIR" <<'PY'
import datetime
import glob
import os
import sys
import tempfile

applied, check_only, lib_dir = sys.argv[1], sys.argv[2] == "1", sys.argv[3]
sys.path.insert(0, lib_dir)
import applied_md_lib as aml  # noqa: E402  (sibling module; path set above)


def write_atomic(path, text):
    # Write to a temp sibling and os.replace() so a crash mid-write never
    # truncates the target; the partition/head pair stays retry-safe together
    # with the duplicate-drop in the partition merge below.
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path) or ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def render(header, blocks):
    out = ["".join(header).rstrip("\n") + "\n"]
    for block in blocks:
        out.append("\n" + "".join(block).rstrip("\n") + "\n")
    return "".join(out)


catdir = os.path.dirname(applied)

with open(applied, encoding="utf-8") as f:
    header, entries = aml.split_entries(f.readlines())

today_env = os.environ.get("ROTATE_APPLIED_TODAY", "")
today = (datetime.datetime.strptime(today_env, "%Y-%m-%d").date()
         if today_env else datetime.date.today())
prev_last = today.replace(day=1) - datetime.timedelta(days=1)
keep = {f"{today.year:04d}-{today.month:02d}",
        f"{prev_last.year:04d}-{prev_last.month:02d}"}


def home_of(date):
    """Where a block belongs: "head", or the YYYY-MM of its partition.

    An undated block has no month to be filed under, so it is never moved:
    it stays in the head, and one already in a partition stays there.
    """
    if date is None or date[:7] in keep:
        return "head"
    return date[:7]


# Every existing partition, parsed with the same splitter as the head.
partitions = {}
for path in sorted(glob.glob(os.path.join(catdir, "applied-[0-9][0-9][0-9][0-9]-[0-9][0-9].md"))):
    month = os.path.basename(path)[len("applied-"):-len(".md")]
    with open(path, encoding="utf-8") as f:
        partitions[month] = aml.split_entries(f.readlines())

stale = [(date, block) for date, block in entries if home_of(date) != "head"]
misfiled = [(month, date, block)
            for month, (_, blocks) in sorted(partitions.items())
            for date, block in blocks
            if date is not None and home_of(date) != month]

if not stale and not misfiled:
    print("rotate-applied-md: head is bounded (nothing older than the previous month); "
          "every partition entry sits under its own month.")
    sys.exit(0)
if check_only:
    msg = "rotate-applied-md: rotation due —"
    if stale:
        months = sorted({d[:7] for d, _ in stale})
        msg += f" {len(stale)} entr(ies) from {', '.join(months)} still in {applied};"
    if misfiled:
        msg += f" {len(misfiled)} partition entr(ies) filed under another month;"
    print(msg + " run `bash agents/scripts/core/rotate-applied-md.sh`.")
    sys.exit(1)

PARTITION_HEADER = """# Agent self-improvement — applied (archive partition {month})

> Rotated slice of [`applied.md`](applied.md) (see its header for format /
> categories / workflow). Entries whose original surface date falls in
> {month}, sorted latest first. Append-only via
> `agents/scripts/core/rotate-applied-md.sh`; do not file new work here.
>
> **Deleted-runtime banner (2026-05-21)** — entries below that reference the
> agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`,
> `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`,
> `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`,
> `CiFailureClassifier`, the `dispatch_source` enum, the sentinel-file protocol,
> `agent/<proposalId>` worktrees, the `coderabbit-react-loop` design,
> `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code
> removed 2026-05-21 (v1 PR1 of `../../plans/shipped/github-tracker-backend.md`,
> merge sha `b1d241bc`). Preserved as historical record of what was tried.

<!-- Latest first. Appended by rotate-applied-md.sh only. -->
"""

# Blocks bound for each partition (the head's stale entries, then misfiled
# blocks from other partitions) and blocks bound back for the head.
incoming, to_head, leaving = {}, [], {}
for date, block in stale:
    incoming.setdefault(home_of(date), []).append((date, block))
for month, date, block in misfiled:
    leaving.setdefault(month, []).append(block)
    home = home_of(date)
    (to_head if home == "head" else incoming.setdefault(home, [])).append((date, block))

for month in sorted(set(incoming) | set(leaving)):
    part = os.path.join(catdir, f"applied-{month}.md")
    if month in partitions:
        part_header, existing = partitions[month]
    else:
        part_header, existing = [PARTITION_HEADER.format(month=month)], []
    gone = leaving.get(month, [])
    existing = [(d, b) for d, b in existing if not any(b is g for g in gone)]

    # Drop exact copies only — of a block the partition already holds, or of
    # one this run already routed here. A run interrupted between writing the
    # partition and rewriting applied.md leaves the same entries in both files,
    # and an earlier rotation that never trimmed the head left whole months
    # duplicated there. Anything that is not a byte-identical copy is written
    # here, so an entry whose only copy is in the head is never dropped.
    seen = {aml.block_key(b) for _, b in existing}
    fresh = []
    for date, block in incoming.get(month, []):
        key = aml.block_key(block)
        if key not in seen:
            seen.add(key)
            fresh.append((date, block))

    merged = aml.sort_latest_first(existing + fresh)
    write_atomic(part, render(part_header, [b for _, b in merged]))
    skipped = len(incoming.get(month, [])) - len(fresh)
    print(f"rotate-applied-md: {len(fresh)} entr(ies) -> {part}"
          + (f" ({skipped} already present, skipped)" if skipped else "")
          + (f" ({len(gone)} misfiled entr(ies) moved out)" if gone else ""))

kept = [(d, b) for d, b in entries if home_of(d) == "head"]
seen = {aml.block_key(b) for _, b in kept}
rehomed = 0
for date, block in to_head:
    key = aml.block_key(block)
    if key in seen:
        continue
    seen.add(key)
    # Insert ahead of the first older dated entry, so the head stays latest
    # first without re-sorting entries this run did not move.
    at = next((i for i, (d, _) in enumerate(kept) if d is not None and d < date), len(kept))
    kept.insert(at, (date, block))
    rehomed += 1
write_atomic(applied, render(header, [b for _, b in kept]))
print(f"rotate-applied-md: head keeps {len(kept)} entr(ies) ({', '.join(sorted(keep))})"
      + (f", {rehomed} of them re-homed from a partition." if rehomed else "."))
PY
