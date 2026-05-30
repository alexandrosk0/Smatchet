#!/usr/bin/env bash
# coverage.sh — OpenCppCoverage Windows-first; POSIX falls back to lcov+gcov.
#
# OpenCppCoverage is Windows-only. POSIX runners fall back to `lcov+gcov` but
# the gate ships Windows-runner-only per
# docs/plans/shipped/test-suite-expansion-completion.md § Locked decisions.
# Re-evaluate when a POSIX CI runner is provisioned.
#
# Usage:
#   bash scripts/dev/coverage.sh                    # capture coverage + write HTML/XML
#   bash scripts/dev/coverage.sh --xml-only         # CI mode — just Cobertura XML
#   bash scripts/dev/coverage.sh --threshold 70     # exit 1 if line coverage < 70%
#
# Env overrides:
#   SMATCHET_COVERAGE_BUILD_DIR   build dir to read tests from. Default: pick the
#                                 first existing of build/ninja-test-msvc/,
#                                 build/ninja-test-msvc/.
#   SMATCHET_COVERAGE_OUTPUT_DIR  output dir for coverage artifacts. Default: coverage/
#   OPENCPPCOVERAGE_EXE           path to OpenCppCoverage.exe. Default: `OpenCppCoverage`
#                                 from PATH (Chocolatey install lands at
#                                 /c/Program Files/OpenCppCoverage/OpenCppCoverage.exe).
#
# Exit codes:
#   0 — coverage captured successfully (threshold passed if requested)
#   1 — tool failure / threshold not met
#   2 — required binary (test exe or OpenCppCoverage) missing
#
# Local install hint: https://github.com/OpenCppCoverage/OpenCppCoverage/releases
# On MSYS2 / Windows: `choco install opencppcoverage` (CI runner default).
#
# POSIX fallback recipe (DOCUMENTED, NOT WIRED HERE):
#   apt-get install -y lcov          # gcov ships with gcc
#   cmake -B build/cov --preset ninja-test-msvc  # gcov flags already in preset
#   cmake --build build/cov --target SmatchetTests SmatchetLuaTests
#   (cd build/cov && ctest --output-on-failure)
#   lcov --capture --directory build/cov --output-file coverage/coverage.info \
#        --gcov-tool gcov --rc lcov_branch_coverage=1
#   lcov --remove coverage/coverage.info '/usr/*' '*/build/_deps/*' '*/tests/*' \
#        --output-file coverage/coverage.info
#   genhtml coverage/coverage.info --output-directory coverage/html
#   # Cobertura XML for CI artifact: pip install lcov-cobertura; lcov_cobertura ...

set -euo pipefail

cd "$(dirname "$0")/../.."

XML_ONLY=0
THRESHOLD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --xml-only) XML_ONLY=1; shift ;;
        --threshold)
            if [ $# -lt 2 ] || [[ "$2" == -* ]]; then
                echo "FAIL: --threshold requires an integer value" >&2
                exit 2
            fi
            if ! [[ "$2" =~ ^[0-9]+$ ]]; then
                echo "FAIL: --threshold must be an integer (got: $2)" >&2
                exit 2
            fi
            THRESHOLD="$2"
            shift 2
            ;;
        --threshold=*)
            THRESHOLD="${1#--threshold=}"
            if [ -z "$THRESHOLD" ]; then
                echo "FAIL: --threshold= requires an integer value" >&2
                exit 2
            fi
            if ! [[ "$THRESHOLD" =~ ^[0-9]+$ ]]; then
                echo "FAIL: --threshold must be an integer (got: $THRESHOLD)" >&2
                exit 2
            fi
            shift
            ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# Resolve the build dir holding the test binaries.
BUILD_DIR="${SMATCHET_COVERAGE_BUILD_DIR:-}"
if [ -z "$BUILD_DIR" ]; then
    for candidate in build/ninja-test-msvc build/ninja-test-msvc; do
        if [ -d "$candidate" ]; then
            BUILD_DIR="$candidate"
            break
        fi
    done
fi
if [ -z "$BUILD_DIR" ] || [ ! -d "$BUILD_DIR" ]; then
    echo "FAIL: no usable build directory. Configure + build a tests preset first:" >&2
    echo "  cmake -B build/ninja-test-msvc --preset ninja-test-msvc" >&2
    echo "  cmake --build --preset ninja-test-msvc --target SmatchetTests SmatchetLuaTests" >&2
    exit 2
fi

OUTPUT_DIR="${SMATCHET_COVERAGE_OUTPUT_DIR:-coverage}"
mkdir -p "$OUTPUT_DIR"

OCC="${OPENCPPCOVERAGE_EXE:-OpenCppCoverage}"
if ! command -v "$OCC" >/dev/null 2>&1; then
    if [ -x "/c/Program Files/OpenCppCoverage/OpenCppCoverage.exe" ]; then
        OCC="/c/Program Files/OpenCppCoverage/OpenCppCoverage.exe"
    else
        echo "FAIL: OpenCppCoverage not found. Install via Chocolatey:" >&2
        echo "  choco install opencppcoverage" >&2
        echo "Or download from https://github.com/OpenCppCoverage/OpenCppCoverage/releases" >&2
        echo "Or set OPENCPPCOVERAGE_EXE=/path/to/OpenCppCoverage.exe" >&2
        exit 2
    fi
fi

# Locate the test binaries. Both must exist for an honest aggregate; if either
# is missing the user has skipped the build step or named the wrong preset.
TEST_EXE="$BUILD_DIR/tests/SmatchetTests.exe"
LUA_TEST_EXE="$BUILD_DIR/tests/Lua/SmatchetLuaTests.exe"
for exe in "$TEST_EXE" "$LUA_TEST_EXE"; do
    if [ ! -f "$exe" ]; then
        echo "FAIL: $exe not found. Build first:" >&2
        echo "  cmake --build --preset $(basename "$BUILD_DIR") --target SmatchetTests SmatchetLuaTests" >&2
        exit 2
    fi
done

XML_OUT="$OUTPUT_DIR/coverage.xml"
HTML_OUT="$OUTPUT_DIR/coverage-html"
# Per-run binary intermediates so the second OpenCppCoverage run does not
# overwrite the first run's data. The prior single-XML-export approach silently
# dropped SmatchetTests coverage when SmatchetLuaTests ran second.
BIN_TESTS="$OUTPUT_DIR/coverage-tests.bin"
BIN_LUA="$OUTPUT_DIR/coverage-lua.bin"
rm -f "$BIN_TESTS" "$BIN_LUA" "$XML_OUT"

# OpenCppCoverage uses --source to include / exclude paths. We restrict to
# Source/Core/ (excluding UI / ImGui-heavy bits is the threshold flip's job
# downstream; this script captures everything Source/Core for now).
SOURCE_INCLUDE="Source.Core"
MODULE_INCLUDE="Smatchet"  # matches both SmatchetTests.exe and SmatchetLuaTests.exe

OCC_FILTER_ARGS=(
    --sources "$SOURCE_INCLUDE"
    --modules "$MODULE_INCLUDE"
    --excluded_sources "_deps"
    --excluded_sources "tests"
    --excluded_sources "Source.Plugins.Mcp.imgui"
    --excluded_sources "ImGui"
    --excluded_sources "imgui"
)

# Capture each target into its own binary intermediate, then merge both via a
# third invocation that reads --input_coverage and exports the final Cobertura
# XML + optional HTML. This is OpenCppCoverage's only merge surface — without
# it, the second --export_type cobertura overwrites the first.
echo "[coverage] capturing SmatchetTests via $OCC..."
set +e
"$OCC" "${OCC_FILTER_ARGS[@]}" --export_type "binary:$BIN_TESTS" -- "$TEST_EXE" --no-intro --no-version
RC_TESTS=$?
set -e

echo "[coverage] capturing SmatchetLuaTests..."
set +e
"$OCC" "${OCC_FILTER_ARGS[@]}" --export_type "binary:$BIN_LUA" -- "$LUA_TEST_EXE" --no-intro --no-version
RC_LUA=$?
set -e

if [ "$RC_TESTS" -ne 0 ] || [ "$RC_LUA" -ne 0 ]; then
    echo "FAIL: OpenCppCoverage returned non-zero (SmatchetTests=$RC_TESTS, SmatchetLuaTests=$RC_LUA)" >&2
    exit 1
fi

# Merge the two binaries into the final Cobertura (+ optional HTML) report.
# OpenCppCoverage requires at least one runnable child even on a pure merge; we
# attach `cmd.exe /c exit 0` as a no-op carrier so the tool runs without
# re-executing either test exe a third time.
MERGE_EXPORTS=(--export_type "cobertura:$XML_OUT")
if [ "$XML_ONLY" -eq 0 ]; then
    MERGE_EXPORTS+=(--export_type "html:$HTML_OUT")
fi
echo "[coverage] merging binaries -> $XML_OUT..."
set +e
"$OCC" --input_coverage "$BIN_TESTS" --input_coverage "$BIN_LUA" "${MERGE_EXPORTS[@]}" -- cmd.exe /c exit 0
RC_MERGE=$?
set -e
if [ "$RC_MERGE" -ne 0 ]; then
    echo "FAIL: OpenCppCoverage merge returned $RC_MERGE" >&2
    exit 1
fi

if [ ! -s "$XML_OUT" ]; then
    echo "FAIL: $XML_OUT missing or empty after capture" >&2
    exit 1
fi

# Extract the line-rate from Cobertura XML. Cobertura's <coverage line-rate="0.72" .../>
# is a float in [0,1]; we surface a percentage for the threshold compare.
# Pass paths / values via os.environ (NOT string interpolation into `-c` source)
# so a path / rate string cannot break the Python source or run attacker-
# controlled code under set -euo pipefail.
LINE_RATE=$(XML_OUT="$XML_OUT" python -c '
import os, re
with open(os.environ["XML_OUT"]) as f:
    t = f.read()
m = re.search(r"<coverage[^>]*line-rate=\"([0-9.]+)\"", t)
print(m.group(1) if m else "0")
')
PCT=$(LINE_RATE="$LINE_RATE" python -c '
import os
print(int(round(float(os.environ["LINE_RATE"]) * 100)))
')
echo "[coverage] line coverage: ${PCT}% (rate=$LINE_RATE)"
echo "[coverage] cobertura XML: $XML_OUT"
if [ "$XML_ONLY" -eq 0 ]; then
    echo "[coverage] html report:   $HTML_OUT/index.html"
fi

if [ "$THRESHOLD" -gt 0 ]; then
    if [ "$PCT" -lt "$THRESHOLD" ]; then
        echo "FAIL: line coverage ${PCT}% < threshold ${THRESHOLD}%" >&2
        exit 1
    fi
    echo "[coverage] threshold ${THRESHOLD}% met (${PCT}%)"
fi

exit 0
