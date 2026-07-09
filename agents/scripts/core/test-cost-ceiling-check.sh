#!/usr/bin/env bash
# test-cost-ceiling-check.sh — wrapper running cost-ceiling-check.py --selftest
# (under/over/disabled/missing-file/cache-exclusion assertions) plus a smoke of
# the cost-ceiling-nudge.sh fail-open contract (no python-args, empty stdin).
#
# Auto-enrolled by scripts/dev/test-all.sh.
set -u

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"

PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
if [ -z "$PY" ]; then
    echo "test-cost-ceiling-check: SKIP — no working python" >&2
    exit 0
fi

passed=0
failed=0

if "$PY" "$PROJ_DIR/agents/scripts/core/cost-ceiling-check.py" --selftest; then
    passed=$((passed + 1))
else
    echo "test-cost-ceiling-check: FAIL — --selftest reds" >&2
    failed=$((failed + 1))
fi

# Nudge wrapper must exit 0 even with empty stdin + no token log (fail-open).
if printf '{}' | bash "$PROJ_DIR/agents/scripts/core/cost-ceiling-nudge.sh"; then
    passed=$((passed + 1))
else
    echo "test-cost-ceiling-check: FAIL — nudge wrapper did not fail open" >&2
    failed=$((failed + 1))
fi

echo "Passed: $passed  Failed: $failed"
[ "$failed" -eq 0 ]
