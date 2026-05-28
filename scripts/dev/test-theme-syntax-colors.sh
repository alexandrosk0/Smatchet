#!/usr/bin/env bash
# test-theme-syntax-colors.sh — bucket-A doctest wrapper for the per-theme C++ syntax palette.
#
# Asserts that SmatchetTheme::ApplyStyle(theme) writes a per-theme entry into the file-static
# SmatchetThemeSyntaxColors that CppSyntaxHighlight reads. Proves the round-trip plumbing for all
# 6 themes (SmatchetDark / ModernDark / Vs2022Dark / Vs2022Light / HighContrast / NortonCommander) plus the
# cross-theme divergence claim (switching theme actually recolors the syntax palette).
#
# Builds the doctest rig if SmatchetTests.exe is missing, then runs only the SmatchetTheme test
# cases. Final summary line "Passed: N  Failed: M" is consumed by scripts/dev/test-all.sh.
#
# Exit codes:
#   0 — every assertion passed
#   1 — at least one assertion failed
#   2 — build missing and cmake/build failed

set -euo pipefail

command -v cmake >/dev/null 2>&1 || { echo "cmake required" >&2; exit 2; }

cd "$(dirname "$0")/../.."

TESTS_EXE="${SMATCHET_TESTS_EXE:-build/ninja-test-msvc/tests/SmatchetTests.exe}"

if [ ! -x "$TESTS_EXE" ]; then
    echo "SmatchetTests.exe missing at $TESTS_EXE — configuring + building..."
    cmake --preset ninja-test-msvc >/dev/null 2>&1 || { echo "cmake preset failed"; exit 2; }
    cmake --build --preset ninja-test-msvc --target SmatchetTests 2>&1 | tail -5 || {
        echo "build failed"
        exit 2
    }
fi

if [ ! -x "$TESTS_EXE" ]; then
    echo "SmatchetTests.exe still missing after build at $TESTS_EXE"
    exit 2
fi

# doctest --test-case=<pattern> supports glob — restrict to the theme suite so this wrapper does
# not double-count assertions from other test_cases when test-all.sh aggregates.
OUT=$("$TESTS_EXE" --test-case='SmatchetTheme*' --reporters=console 2>&1 || true)
echo "$OUT"

# Parse the doctest summary line — "assertions: N | M passed | K failed | …".
ASSERTIONS_LINE=$(echo "$OUT" | grep -E '^\[doctest\] assertions:' | tail -n 1 || true)
if [ -z "$ASSERTIONS_LINE" ]; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: no doctest assertions summary line found"
    exit 1
fi

PASSED=$(echo "$ASSERTIONS_LINE" | sed -E 's/.*\| *([0-9]+) +passed.*/\1/')
FAILED=$(echo "$ASSERTIONS_LINE" | sed -E 's/.*\| *([0-9]+) +failed.*/\1/')

echo "Passed: $PASSED  Failed: $FAILED"

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
