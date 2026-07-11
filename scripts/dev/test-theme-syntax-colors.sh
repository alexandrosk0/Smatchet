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
#   1 — at least one assertion failed (or the suite vanished — zero-assertion guard, #80)
#   2 — build missing and cmake/build failed
#
# selftest: asserts-failure

set -euo pipefail

# Given doctest console output, print the "Passed: X  Failed: Y" summary line and
# return 0 (all passed) / 1 (a failure OR a ZERO-assertion vanished suite, #80).
evaluate_theme_output() {
    local out="$1" line passed failed
    line=$(printf '%s\n' "$out" | grep -E '^\[doctest\] assertions:' | tail -n 1 || true)
    if [ -z "$line" ]; then
        echo "Passed: 0  Failed: 1"
        echo "ERROR: no doctest assertions summary line found" >&2
        return 1
    fi
    passed=$(printf '%s' "$line" | sed -E 's/.*\| *([0-9]+) +passed.*/\1/')
    failed=$(printf '%s' "$line" | sed -E 's/.*\| *([0-9]+) +failed.*/\1/')
    [[ "$passed" =~ ^[0-9]+$ ]] || passed=0
    [[ "$failed" =~ ^[0-9]+$ ]] || failed=0
    # #80 zero-assertion guard: a `SmatchetTheme*` glob matching ZERO cases still
    # prints "assertions: 0 | 0 passed | 0 failed" — a non-empty line, so the old
    # `failed -gt 0` check let a deleted/renamed suite pass green. A run that
    # exercised no assertions is a vanished suite, not a success.
    if [ "$((passed + failed))" -eq 0 ]; then
        echo "Passed: 0  Failed: 1"
        echo "ERROR: the SmatchetTheme* suite ran ZERO assertions — vanished/renamed? (zero-assertion guard)" >&2
        return 1
    fi
    echo "Passed: $passed  Failed: $failed"
    [ "$failed" -gt 0 ] && return 1
    return 0
}

# --selftest: prove the zero-assertion guard fires, a healthy run passes, and a
# real failure fails — with crafted doctest summary lines (no build/exe needed).
run_selftest() {
    if evaluate_theme_output "[doctest] assertions: 0 | 0 passed | 0 failed |" >/dev/null 2>&1; then
        echo "test-theme-syntax-colors --selftest: FAIL — a zero-assertion (vanished) suite passed" >&2
        return 1
    fi
    if ! evaluate_theme_output "[doctest] assertions: 12 | 12 passed | 0 failed |" >/dev/null 2>&1; then
        echo "test-theme-syntax-colors --selftest: FAIL — a healthy run was rejected" >&2
        return 1
    fi
    if evaluate_theme_output "[doctest] assertions: 12 | 10 passed | 2 failed |" >/dev/null 2>&1; then
        echo "test-theme-syntax-colors --selftest: FAIL — a failing run passed" >&2
        return 1
    fi
    echo "test-theme-syntax-colors --selftest: PASS — zero-assertion guard fires; healthy passes; failures fail."
    return 0
}

if [ "${1:-}" = "--selftest" ]; then
    run_selftest
    exit $?
fi

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

# Parse + guard (incl. the #80 zero-assertion guard) via the testable helper.
evaluate_theme_output "$OUT" && rc=0 || rc=$?
exit "$rc"
