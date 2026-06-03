#!/usr/bin/env bash
# test-backlog-counts.sh — self-improvement backlog count tool + regression guard.
#
# History: this used to VERIFY a stored "Live count" column in
# docs/self-improvement/AGENT_SELF_IMPROVEMENT.md against the actual entry counts.
# That stored count was edited by EVERY entry-adding PR, so concurrent PRs
# conflicted on that one line on every add — the highest-frequency self-improvement
# merge conflict. The column was removed 2026-06-03; counts are now on-demand.
#
# So this script now:
#   default  — GUARD: fail if a stored numeric count column was re-introduced
#              into the § Index table (regression of the conflict-prone pattern).
#   --list   — print the live per-category counts (grep -c '^- 20' <file>).
#
# Bypass: SMATCHET_SKIP_BACKLOG_COUNTS=1 (logged when used).
#
# Exit codes:
#   0 — guard clean (or --list printed)
#   1 — a stored numeric count column was re-introduced (conflict-prone)
#   2 — missing index / category file

set -euo pipefail
cd "$(dirname "$0")/../../.."

if [ "${SMATCHET_SKIP_BACKLOG_COUNTS:-0}" = "1" ]; then
    echo "test-backlog-counts: SMATCHET_SKIP_BACKLOG_COUNTS=1 — skipping" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

INDEX="docs/self-improvement/AGENT_SELF_IMPROVEMENT.md"
DIR="docs/self-improvement/categories"
[ -f "$INDEX" ] || { echo "missing index: $INDEX" >&2; exit 2; }

# label -> category file basename.
declare -A FILES=(
    [bug]=bug
    [debt]=debt
    [process]=process
    [tooling]=tooling
    [infra]=infra
    [test]=test
    [security]=security
    [external]=external-blockers
    [applied]=applied
)

# --list: print live counts on demand (the replacement for the stored column).
if [ "${1:-}" = "--list" ]; then
    for label in bug debt process tooling infra test security external applied; do
        f="$DIR/${FILES[$label]}.md"
        [ -f "$f" ] || { echo "missing category file: $f" >&2; exit 2; }
        printf '%-10s %s\n' "$label" "$(grep -c '^- 20' "$f" 2>/dev/null || echo 0)"
    done
    exit 0
fi

# default: regression guard — the § Index table row shape is `| <label> | <file> |`
# (2 columns). A re-introduced stored count makes the 2nd cell a bare integer.
offenders="$(awk -F'|' '
    /^\| *(bug|process|tooling|infra|test|security|external|applied)/ {
        cell = $3
        gsub(/^[ \t]+|[ \t]+$/, "", cell)
        if (cell ~ /^[0-9]+$/) {
            label = $2
            gsub(/^[ \t]+|[ \t]+$/, "", label)
            print label
        }
    }
' "$INDEX")"

if [ -n "$offenders" ]; then
    echo "test-backlog-counts: a STORED numeric count column was re-introduced into" >&2
    echo "  $INDEX § Index — this is the conflict-prone pattern removed 2026-06-03." >&2
    echo "  Offending row label(s): $offenders" >&2
    echo "  Counts are on-demand: drop the column, use 'test-backlog-counts.sh --list'." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "test-backlog-counts: no stored count column (counts are on-demand via --list)."
echo "Passed: 1  Failed: 0"
exit 0
