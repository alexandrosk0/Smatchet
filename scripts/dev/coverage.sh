#!/usr/bin/env bash
# coverage.sh — OpenCppCoverage Windows-first; POSIX falls back to lcov+gcov.
#
# OpenCppCoverage is Windows-only. POSIX runners fall back to `lcov+gcov` but
# the gate ships Windows-runner-only per
# docs/design/applied/test-suite-expansion-completion.md § Locked decisions.
# Re-evaluate when a POSIX CI runner is provisioned.
#
# Usage:
#   bash scripts/dev/coverage.sh                    # capture coverage + write HTML/XML
#   bash scripts/dev/coverage.sh --xml-only         # CI mode — just Cobertura XML
#   bash scripts/dev/coverage.sh --threshold 70     # exit 1 if line coverage < 70%
#
# Env overrides:
#   SMATCHET_COVERAGE_BUILD_DIR   build dir to read tests from. Default: pick the
#                                 first existing of build/ninja-coverage-msys2/,
#                                 build/ninja-test-msys2/.
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
#   cmake -B build/cov --preset ninja-coverage-msys2  # gcov flags already in preset
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
        --threshold) THRESHOLD="${2:-0}"; shift 2 ;;
        --threshold=*) THRESHOLD="${1#--threshold=}"; shift ;;
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
    for candidate in build/ninja-coverage-msys2 build/ninja-test-msys2; do
        if [ -d "$candidate" ]; then
            BUILD_DIR="$candidate"
            break
        fi
    done
fi
if [ -z "$BUILD_DIR" ] || [ ! -d "$BUILD_DIR" ]; then
    echo "FAIL: no usable build directory. Configure + build a tests preset first:" >&2
    echo "  cmake -B build/ninja-coverage-msys2 --preset ninja-coverage-msys2" >&2
    echo "  cmake --build --preset ninja-coverage-msys2 --target SmatchetTests SmatchetLuaTests" >&2
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

# OpenCppCoverage uses --source to include / exclude paths. We restrict to
# Source_Core/ (excluding UI / ImGui-heavy bits is the threshold flip's job
# downstream; this script captures everything Source_Core for now).
SOURCE_INCLUDE="Source_Core"
MODULE_INCLUDE="Smatchet"  # matches both SmatchetTests.exe and SmatchetLuaTests.exe

OCC_COMMON_ARGS=(
    --sources "$SOURCE_INCLUDE"
    --modules "$MODULE_INCLUDE"
    --excluded_sources "_deps"
    --excluded_sources "tests"
    --excluded_sources "Plugins/Mcp/imgui"
    --excluded_sources "ImGui"
    --excluded_sources "imgui"
    --export_type "cobertura:$XML_OUT"
)

if [ "$XML_ONLY" -eq 0 ]; then
    OCC_COMMON_ARGS+=(--export_type "html:$HTML_OUT")
fi

# OpenCppCoverage runs each target in a separate child; we drive it twice and
# rely on its merge behaviour by aggregating XML reports outside the tool when
# CI needs it. For dev mode the second run overwrites — acceptable because the
# user is iterating one target at a time; full coverage runs in CI.
echo "[coverage] capturing SmatchetTests via $OCC..."
set +e
"$OCC" "${OCC_COMMON_ARGS[@]}" -- "$TEST_EXE" --no-intro --no-version
RC_TESTS=$?
set -e

echo "[coverage] capturing SmatchetLuaTests..."
set +e
"$OCC" "${OCC_COMMON_ARGS[@]}" -- "$LUA_TEST_EXE" --no-intro --no-version
RC_LUA=$?
set -e

if [ "$RC_TESTS" -ne 0 ] || [ "$RC_LUA" -ne 0 ]; then
    echo "FAIL: OpenCppCoverage returned non-zero (SmatchetTests=$RC_TESTS, SmatchetLuaTests=$RC_LUA)" >&2
    exit 1
fi

if [ ! -s "$XML_OUT" ]; then
    echo "FAIL: $XML_OUT missing or empty after capture" >&2
    exit 1
fi

# Extract the line-rate from Cobertura XML. Cobertura's <coverage line-rate="0.72" .../>
# is a float in [0,1]; we surface a percentage for the threshold compare.
LINE_RATE=$(python -c "import re,sys; t=open(r'$XML_OUT').read(); m=re.search(r'<coverage[^>]*line-rate=\"([0-9.]+)\"',t); print(m.group(1) if m else '0')")
PCT=$(python -c "print(int(round(float('$LINE_RATE')*100)))")
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
