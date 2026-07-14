#!/usr/bin/env bash
# agents/scripts/core/sort-applied-md.sh — restore "Latest first" ordering on
# applied.md after a `merge=union` driver concatenation interleaves dates.
#
# Companion to the `merge=union` driver in `.gitattributes` (per
# docs/agent-rules/process-rules.md § Backlog-archive union merge). The driver
# concatenates parallel prepends verbatim — date order may interleave on the
# merge commit. This script re-sorts entries by their YYYY-MM-DD prefix
# descending while preserving:
#   - the file header (lines before the first entry)
#   - each entry's multi-line block (Resolution lines after the title)
#   - blank-line separators between entries
#
# Usage:
#   bash agents/scripts/core/sort-applied-md.sh
#   bash agents/scripts/core/sort-applied-md.sh --check    # exit 1 if reorder needed
#
# Exit codes:
#   0 — applied.md sorted (or already sorted in --check)
#   1 — --check mode, file is out of order; OR Python parse error propagated
#       via `set -e` (cannot distinguish — both surface as exit 1)
#   2 — file not found

set -euo pipefail

command -v python3 >/dev/null 2>&1 || { echo "python3 required" >&2; exit 2; }

cd "$(dirname "$0")/../../.."

APPLIED="docs/self-improvement/categories/applied.md"
CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
fi

if [ ! -f "$APPLIED" ]; then
    echo "sort-applied-md: $APPLIED not found" >&2
    exit 2
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

# Read the file. Lines before the first "- YYYY-MM-DD" entry are the header
# (kept verbatim at the top). Each entry begins with "- 2026-XX-XX ..." and
# continues until the next entry or EOF (Resolution / Status / Last-reviewed
# continuation lines are indented or blank-separated).
python3 - "$APPLIED" "$TMP" <<'PY'
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, encoding="utf-8") as f:
    lines = f.readlines()

# Split: header (everything up to first entry) + list of entries.
# An entry starts with "- YYYY-MM-DD ".
entry_re = re.compile(r"^- (\d{4}-\d{2}-\d{2}) ")
header_end = 0
for i, line in enumerate(lines):
    if entry_re.match(line):
        header_end = i
        break
else:
    # No entries — nothing to sort.
    with open(dst, "w", encoding="utf-8") as f:
        f.writelines(lines)
    sys.exit(0)

header = lines[:header_end]

# Collect entries. Each entry is the entry-line + all following lines until
# (a) the next entry-line, or (b) EOF.
entries = []
current = []
current_date = None
for line in lines[header_end:]:
    m = entry_re.match(line)
    if m:
        if current:
            entries.append((current_date, current))
        current = [line]
        current_date = m.group(1)
    else:
        current.append(line)
if current:
    entries.append((current_date, current))

# Sort descending by date prefix. Stable sort preserves intra-date order.
entries.sort(key=lambda e: e[0], reverse=True)

with open(dst, "w", encoding="utf-8") as f:
    f.writelines(header)
    for _, block in entries:
        f.writelines(block)
PY

if [ "$CHECK_ONLY" -eq 1 ]; then
    if cmp -s "$APPLIED" "$TMP"; then
        echo "sort-applied-md: $APPLIED is sorted"
        exit 0
    else
        echo "sort-applied-md: $APPLIED is NOT sorted (run without --check to fix)" >&2
        exit 1
    fi
fi

if cmp -s "$APPLIED" "$TMP"; then
    echo "sort-applied-md: $APPLIED already sorted; no changes"
    exit 0
fi

mv "$TMP" "$APPLIED"
trap - EXIT
echo "sort-applied-md: restored Latest-first ordering on $APPLIED"
