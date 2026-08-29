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
# Partition files are created with a standard header (including the
# deleted-runtime banner, which is self-scoping: it annotates only entries
# that reference the removed agentic-flow C++ runtime) and are sorted latest
# first, like the head. Rotation appends to an existing partition and re-sorts
# it. Idempotent: a second run is a no-op.
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
# Exit codes:
#   0 — rotated (or nothing to rotate; or --check with nothing due)
#   1 — --check mode and rotation is due; OR Python error via `set -e`
#   2 — applied.md not found / no python

set -euo pipefail

# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "python3 required (no working interpreter on PATH)" >&2; exit 2; }

cd "$(dirname "$0")/../../.."

APPLIED="docs/self-improvement/categories/applied.md"
CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
fi

if [ ! -f "$APPLIED" ]; then
    echo "rotate-applied-md: $APPLIED not found" >&2
    exit 2
fi

"$PY" - "$APPLIED" "$CHECK_ONLY" <<'PY'
import datetime
import os
import re
import sys

applied, check_only = sys.argv[1], sys.argv[2] == "1"
catdir = os.path.dirname(applied)

with open(applied, encoding="utf-8") as f:
    lines = f.readlines()

entry_re = re.compile(r"^- (\d{4})-(\d{2})-\d{2} ")

# Split header (everything before the first entry) from entry blocks.
header, entries, cur, cur_month = [], [], None, None
for line in lines:
    m = entry_re.match(line)
    if m:
        if cur is not None:
            entries.append((cur_month, cur))
        cur, cur_month = [line], (int(m.group(1)), int(m.group(2)))
    elif cur is None:
        header.append(line)
    else:
        cur.append(line)
if cur is not None:
    entries.append((cur_month, cur))

today = datetime.date.today()
keep = {(today.year, today.month)}
prev_last = today.replace(day=1) - datetime.timedelta(days=1)
keep.add((prev_last.year, prev_last.month))

stale = [(month, block) for month, block in entries if month not in keep]
if not stale:
    print("rotate-applied-md: head is bounded (nothing older than the previous month).")
    sys.exit(0)
if check_only:
    months = sorted({f"{y:04d}-{m:02d}" for (y, m), _ in stale})
    print(f"rotate-applied-md: rotation due — {len(stale)} entr(ies) from {', '.join(months)} "
          f"still in {applied}; run `bash agents/scripts/core/rotate-applied-md.sh`.")
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


def entry_date(block):
    m = re.match(r"^- (\d{4}-\d{2}-\d{2})", block[0])
    return m.group(1) if m else "0000-00-00"


by_month = {}
for (y, m), block in stale:
    by_month.setdefault(f"{y:04d}-{m:02d}", []).append(block)

for month, blocks in sorted(by_month.items()):
    part = os.path.join(catdir, f"applied-{month}.md")
    existing = []
    if os.path.exists(part):
        with open(part, encoding="utf-8") as f:
            plines = f.readlines()
        phead, pcur = [], None
        for line in plines:
            if entry_re.match(line):
                if pcur is not None:
                    existing.append(pcur)
                pcur = [line]
            elif pcur is None:
                phead.append(line)
            else:
                pcur.append(line)
        if pcur is not None:
            existing.append(pcur)
        part_header = "".join(phead)
    else:
        part_header = PARTITION_HEADER.format(month=month)

    merged = existing + blocks
    merged.sort(key=entry_date, reverse=True)
    with open(part, "w", encoding="utf-8") as f:
        f.write(part_header.rstrip("\n") + "\n")
        for block in merged:
            body = "".join(block).rstrip("\n")
            f.write("\n" + body + "\n")
    print(f"rotate-applied-md: {len(blocks)} entr(ies) -> {part}")

kept = [block for month, block in entries if month in keep]
with open(applied, "w", encoding="utf-8") as f:
    f.write("".join(header).rstrip("\n") + "\n")
    for block in kept:
        body = "".join(block).rstrip("\n")
        f.write("\n" + body + "\n")
print(f"rotate-applied-md: head keeps {len(kept)} entr(ies) "
      f"({', '.join(sorted(f'{y:04d}-{m:02d}' for y, m in keep))}).")
PY
