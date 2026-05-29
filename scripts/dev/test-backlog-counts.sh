#!/usr/bin/env bash
# test-backlog-counts.sh — verify (or fix) the Live-count column in
# docs/self-improvement/AGENT_SELF_IMPROVEMENT.md matches the actual entry counts
# in each self-improvement/categories/<category>.md.
#
# Why: the index table is a manually-maintained summary with no documented
# update rule. Drift was substantial at first inspection (2026-05-28
# session: bug 6→11, process 14→27, tooling 26→32, infra 10→13, test 9→18,
# security 6→10, external 3→1, applied 75→138). This lint makes drift fail
# CI, closing the rule-not-documented gap behind the drift.
#
# Modes:
#   default       — verify counts match; fail with diff if any row differs.
#   --fix         — rewrite each "Live count" cell from the actual count.
#
# Bypass: SMATCHET_SKIP_BACKLOG_COUNTS=1 (logged when used).
#
# Exit codes:
#   0 — every row matches (or --fix wrote changes)
#   1 — at least one row drifted
#   2 — missing index file or category file

set -euo pipefail
cd "$(dirname "$0")/../.."

if [ "${SMATCHET_SKIP_BACKLOG_COUNTS:-0}" = "1" ]; then
    echo "test-backlog-counts: SMATCHET_SKIP_BACKLOG_COUNTS=1 — skipping" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

INDEX="docs/self-improvement/AGENT_SELF_IMPROVEMENT.md"
DIR="docs/self-improvement/categories"
[ -f "$INDEX" ] || { echo "missing index: $INDEX" >&2; exit 2; }

FIX=0
if [ "${1:-}" = "--fix" ]; then FIX=1; fi

# (label-in-index, file-basename) pairs — labels match the index table cells.
declare -A FILES=(
    [bug]=bug
    [process]=process
    [tooling]=tooling
    [infra]=infra
    [test]=test
    [security]=security
    [external]=external-blockers
)
APPLIED_BASENAME=applied

# Compute actual counts.
declare -A ACTUAL=()
for label in "${!FILES[@]}"; do
    f="$DIR/${FILES[$label]}.md"
    [ -f "$f" ] || { echo "missing category file: $f" >&2; exit 2; }
    ACTUAL[$label]=$(grep -c '^- 20' "$f" 2>/dev/null || echo 0)
done
APPLIED_FILE="$DIR/$APPLIED_BASENAME.md"
[ -f "$APPLIED_FILE" ] || { echo "missing category file: $APPLIED_FILE" >&2; exit 2; }
ACTUAL_APPLIED=$(grep -c '^- 20' "$APPLIED_FILE" 2>/dev/null || echo 0)

DRIFT=()
PASS=0; FAIL=0

# Parse the current index table — match the `| label | count | …` shape.
check_row() {
    local label="$1" expected="$2"
    local current
    # Use a tolerant regex that allows variable inner spacing.
    current=$(awk -v lbl="$label" -F'|' '
        {
            # Trim spaces from columns 2 + 3.
            l = $2; gsub(/^ +| +$/, "", l)
            if (l == lbl) {
                v = $3; gsub(/^ +| +$/, "", v)
                print v
                exit
            }
        }
    ' "$INDEX")
    if [ -z "$current" ]; then
        DRIFT+=("$label: row NOT FOUND in index")
        FAIL=$((FAIL+1))
        return
    fi
    if [ "$current" != "$expected" ]; then
        DRIFT+=("$label: index says $current, actual $expected")
        FAIL=$((FAIL+1))
    else
        PASS=$((PASS+1))
    fi
}

for label in bug process tooling infra test security external; do
    check_row "$label" "${ACTUAL[$label]}"
done
check_row "applied (archive)" "$ACTUAL_APPLIED"

if [ "$FAIL" -eq 0 ]; then
    echo "Passed: $PASS  Failed: 0"
    exit 0
fi

echo "test-backlog-counts: drift detected:" >&2
for d in "${DRIFT[@]}"; do echo "  $d" >&2; done

if [ "$FIX" -eq 0 ]; then
    echo "" >&2
    echo "Re-run with --fix to rewrite the index table from actual file counts." >&2
    echo "Passed: $PASS  Failed: $FAIL"
    exit 1
fi

# --fix mode: rewrite each row via python — handles variable column padding
# correctly and avoids shell-quoting-perl-backrefs landmines.
# Pick a working interpreter. The Windows Store `python3` shim satisfies
# `command -v` but errors at runtime ("Python was not found"), so probe each
# candidate by actually running it before committing to it.
PYBIN=""
for _cand in python3 python py; do
    if command -v "$_cand" >/dev/null 2>&1 && [ "$("$_cand" -c 'print("ok")' 2>/dev/null)" = "ok" ]; then
        PYBIN="$_cand"; break
    fi
done
[ -n "$PYBIN" ] || { echo "python required for --fix (tried python3/python/py; Windows Store shim does not count)" >&2; exit 2; }
"$PYBIN" - "$INDEX" "${ACTUAL[bug]}" "${ACTUAL[process]}" "${ACTUAL[tooling]}" \
    "${ACTUAL[infra]}" "${ACTUAL[test]}" "${ACTUAL[security]}" \
    "${ACTUAL[external]}" "$ACTUAL_APPLIED" <<'PY'
import re, sys
path = sys.argv[1]
counts = {
    "bug": sys.argv[2],
    "process": sys.argv[3],
    "tooling": sys.argv[4],
    "infra": sys.argv[5],
    "test": sys.argv[6],
    "security": sys.argv[7],
    "external": sys.argv[8],
    "applied (archive)": sys.argv[9],
}
text = open(path, encoding="utf-8").read()
out_lines = []
for line in text.splitlines():
    # Match `| <label> | <count> | ...` table rows.
    m = re.match(r"^(\|\s*)([^|]+?)(\s*\|\s*)(\d+)(\s*\|.*)$", line)
    if m:
        label = m.group(2).strip()
        if label in counts:
            line = m.group(1) + m.group(2) + m.group(3) + counts[label] + m.group(5)
    out_lines.append(line)
open(path, "w", encoding="utf-8").write("\n".join(out_lines) + "\n")
PY
echo "test-backlog-counts: index rewritten; re-validating..." >&2

# Re-validate to ensure missing rows weren't silently left unresolved.
DRIFT=()
PASS=0
FAIL=0
for label in bug process tooling infra test security external; do
    check_row "$label" "${ACTUAL[$label]}"
done
check_row "applied (archive)" "$ACTUAL_APPLIED"

if [ "$FAIL" -eq 0 ]; then
    echo "Passed: $PASS  Failed: 0"
    exit 0
fi
echo "test-backlog-counts: --fix incomplete; unresolved drift remains:" >&2
for d in "${DRIFT[@]}"; do echo "  $d" >&2; done
echo "Passed: $PASS  Failed: $FAIL"
exit 1
