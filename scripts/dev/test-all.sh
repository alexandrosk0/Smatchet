#!/usr/bin/env bash
# test-all.sh — run every scripts/dev/test-*.sh and aggregate Passed/Failed.
#
# Discovers tests by glob: scripts/dev/test-*.sh (excludes self).
# Each test script must follow the test-author bash conventions:
#   - set -euo pipefail
#   - exits 0 on all-pass, 1 on assertion failure, 2 on missing binary/build.
#   - prints a final "Passed: N  Failed: M" line (regex-extracted here).
#
# Env overrides forwarded to children: SMATCHET_EXE, SMATCHET_TEST_PORT, PYTHON.
#
# Exit codes:
#   0 — every individual script reported Failed: 0
#   1 — at least one script reported Failed: >0 OR exited non-zero
#   2 — no test scripts found OR a script exited 2 (missing binary)
#
# Usage:
#   bash scripts/dev/test-all.sh                     # all tests
#   bash scripts/dev/test-all.sh --filter lua        # only test-*lua*.sh

set -euo pipefail

cd "$(dirname "$0")/../.."

FILTER=""
if [ "${1:-}" = "--filter" ]; then
    FILTER="${2:-}"
    shift 2
fi

# Collect test scripts.
mapfile -t TESTS < <(find scripts/dev -maxdepth 1 -type f -name 'test-*.sh' ! -name 'test-all.sh' | sort)

if [ "${#TESTS[@]}" -eq 0 ]; then
    echo "no tests found under scripts/dev/test-*.sh" >&2
    exit 2
fi

if [ -n "$FILTER" ]; then
    FILTERED=()
    for t in "${TESTS[@]}"; do
        case "$t" in
            *"$FILTER"*) FILTERED+=("$t") ;;
        esac
    done
    TESTS=("${FILTERED[@]}")
    if [ "${#TESTS[@]}" -eq 0 ]; then
        echo "no tests matched --filter $FILTER" >&2
        exit 2
    fi
fi

TOTAL_PASSED=0
TOTAL_FAILED=0
FAILED_SCRIPTS=()
MISSING_BINARY=0

for script in "${TESTS[@]}"; do
    echo
    echo "##################################################"
    echo "# $script"
    echo "##################################################"

    # Capture output but stream to terminal too.
    OUT=$(bash "$script" 2>&1) || RC=$?
    RC="${RC:-0}"
    echo "$OUT"

    if [ "$RC" -eq 2 ]; then
        MISSING_BINARY=1
        FAILED_SCRIPTS+=("$script (missing binary/build, exit=2)")
        continue
    fi

    # Extract Passed: / Failed: from final summary line.
    SUMMARY=$(echo "$OUT" | grep -E '^Passed: [0-9]+  Failed: [0-9]+' | tail -n 1 || true)
    if [ -z "$SUMMARY" ]; then
        FAILED_SCRIPTS+=("$script (no summary line, exit=$RC)")
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        continue
    fi

    P=$(echo "$SUMMARY" | sed -E 's/^Passed: ([0-9]+).*/\1/')
    F=$(echo "$SUMMARY" | sed -E 's/.*Failed: ([0-9]+).*/\1/')
    TOTAL_PASSED=$((TOTAL_PASSED + P))
    TOTAL_FAILED=$((TOTAL_FAILED + F))
    if [ "$F" -gt 0 ] || [ "$RC" -ne 0 ]; then
        FAILED_SCRIPTS+=("$script ($F assertion failure(s), exit=$RC)")
    fi
done

echo
echo "============================================================"
echo "AGGREGATE  Passed: $TOTAL_PASSED  Failed: $TOTAL_FAILED  Scripts: ${#TESTS[@]}"
echo "============================================================"

if [ "$MISSING_BINARY" -eq 1 ]; then
    echo
    echo "Missing binary — rebuild and retry:"
    echo "  cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone"
    exit 2
fi

if [ "${#FAILED_SCRIPTS[@]}" -gt 0 ]; then
    echo
    echo "Failed scripts:"
    for f in "${FAILED_SCRIPTS[@]}"; do
        echo "  - $f"
    done
    exit 1
fi

exit 0
