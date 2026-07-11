#!/usr/bin/env bash
# test-markdown-lang-tag.sh — bucket-A doctest wrapper for the markdown fenced-code lang classifier.
#
# Asserts that MarkdownPreviewRender::IsCppLikeLangTag recognises the canonical C/C++ spellings
# (cpp / c++ / cxx / cc / c / hpp / h), is case-insensitive, does not prefix-match (so cppreference
# / ccache / cxxabi stay plain), and rejects every non-C/C++ language tested. This is the seam
# routing ` ```cpp ` markdown fences into DrawColoredCppLine; covering it as bucket A means a
# future bug like "lowercase the wrong slice" or "switch on substring" gets caught at unit-test
# time without the full markdown-render harness.
#
# Builds the doctest rig if SmatchetTests.exe is missing, then runs only the IsCppLikeLangTag
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

OUT=$("$TESTS_EXE" --test-case='IsCppLikeLangTag*' --reporters=console 2>&1 || true)
echo "$OUT"

ASSERTIONS_LINE=$(echo "$OUT" | grep -E '^\[doctest\] assertions:' | tail -n 1 || true)
if [ -z "$ASSERTIONS_LINE" ]; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: no doctest assertions summary line found"
    exit 1
fi

PASSED=$(echo "$ASSERTIONS_LINE" | sed -E 's/.*\| *([0-9]+) +passed.*/\1/')
FAILED=$(echo "$ASSERTIONS_LINE" | sed -E 's/.*\| *([0-9]+) +failed.*/\1/')

echo "Passed: $PASSED  Failed: $FAILED"

# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
