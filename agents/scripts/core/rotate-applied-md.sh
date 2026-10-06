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
# Crash-safe ordering: every block that moves is written into its DESTINATION
# (a partition, or the head) before any file drops it from its SOURCE, so a
# crash between two writes can leave a block in two files but never in none —
# and the exact-copy dedupe folds such a copy back to one on the next run.
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
#      ROTATE_APPLIED_CRASH_AFTER=N      test seam: exit 3 right after the Nth
#                                        file write (a simulated crash).
#
# Exit codes:
#   0 — rotated (or nothing to rotate; or --check with nothing due)
#   1 — --check mode and rotation is due; OR Python error via `set -e`
#   2 — applied.md not found / no python
#   3 — the ROTATE_APPLIED_CRASH_AFTER test seam fired

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


_crash_env = os.environ.get("ROTATE_APPLIED_CRASH_AFTER", "")
CRASH_AFTER = int(_crash_env) if _crash_env.isdigit() else 0
_writes = 0


def write_atomic(path, text):
    # Write to a temp sibling and os.replace() so a crash mid-write never
    # truncates the target. Crashes BETWEEN writes are covered by the
    # destination-first write order below plus the exact-copy dedupe.
    global _writes
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
    _writes += 1
    if CRASH_AFTER and _writes >= CRASH_AFTER:
        print(f"rotate-applied-md: simulated crash after {_writes} write(s)", file=sys.stderr)
        sys.exit(3)


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

# Plan every touched file's content before writing any of it. A file that both
# gains and loses blocks gets an interim "union" text (its current blocks plus
# the additions) for phase 1; its final text (the losses dropped) is phase 2.
plans = []   # (path, union text or None, final text, gains blocks?)
for month in sorted(set(incoming) | set(leaving)):
    part = os.path.join(catdir, f"applied-{month}.md")
    if month in partitions:
        part_header, current = partitions[month]
    else:
        part_header, current = [PARTITION_HEADER.format(month=month)], []
    gone = leaving.get(month, [])
    existing = [(d, b) for d, b in current if not any(b is g for g in gone)]

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

    final = render(part_header, [b for _, b in aml.sort_latest_first(existing + fresh)])
    union = (render(part_header, [b for _, b in aml.sort_latest_first(current + fresh)])
             if fresh and gone else None)
    plans.append((part, union, final, bool(fresh)))
    skipped = len(incoming.get(month, [])) - len(fresh)
    print(f"rotate-applied-md: {len(fresh)} entr(ies) -> {part}"
          + (f" ({skipped} already present, skipped)" if skipped else "")
          + (f" ({len(gone)} misfiled entr(ies) moved out)" if gone else ""))


def with_rehomed(base):
    """`base` plus every to_head block it lacks; returns (blocks, count added)."""
    out = list(base)
    seen = {aml.block_key(b) for _, b in out}
    added = 0
    for date, block in to_head:
        key = aml.block_key(block)
        if key in seen:
            continue
        seen.add(key)
        # Insert ahead of the first older dated entry, so the head stays latest
        # first without re-sorting entries this run did not move.
        at = next((i for i, (d, _) in enumerate(out) if d is not None and d < date), len(out))
        out.insert(at, (date, block))
        added += 1
    return out, added


kept, rehomed = with_rehomed([(d, b) for d, b in entries if home_of(d) == "head"])
head_final = render(header, [b for _, b in kept])
head_union = (render(header, [b for _, b in with_rehomed(entries)[0]])
              if rehomed and stale else None)

# Phase 1 — additions: every destination gains its blocks while every source
# still holds them. Phase 2 — removals: the final texts. A file with only
# additions is final after phase 1; one with only removals waits for phase 2.
for part, union, final, gains in plans:
    if gains:
        write_atomic(part, union if union is not None else final)
if rehomed:
    write_atomic(applied, head_union if head_union is not None else head_final)
for part, union, final, gains in plans:
    if not gains or union is not None:
        write_atomic(part, final)
if not rehomed or head_union is not None:
    write_atomic(applied, head_final)
print(f"rotate-applied-md: head keeps {len(kept)} entr(ies) ({', '.join(sorted(keep))})"
      + (f", {rehomed} of them re-homed from a partition." if rehomed else "."))
PY
