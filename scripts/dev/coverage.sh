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
#                                 Also the seam tests/bats/coverage_gate.bats stubs.
#
# Exit codes:
#   0 — coverage captured successfully (threshold passed if requested)
#   1 — a test binary failed under capture / threshold not met (fix the code)
#   2 — required binary (test exe or OpenCppCoverage) missing
#   3 — coverage INFRA failure (fix nothing, re-run): OpenCppCoverage produced no
#       coverage data, or the merge wrote no usable coverage.xml, even after one
#       automatic retry. Marked by a `::error title=COVERAGE-INFRA-CRASH::` line so
#       an instrumentation crash never reads as a 0% coverage regression.
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

# classify_capture_failure <label> <rc> <bin-path>
# Distinguish a TEST-BINARY failure (the doctest child returned non-zero — a real test
# regression OpenCppCoverage faithfully forwarded; the .bin intermediate WAS written) from
# an OpenCppCoverage TOOLING failure (the tool could not attach / produced no coverage data;
# no .bin intermediate exists). The two demand different operator action: a test failure means
# "fix the failing test"; a tooling failure means "fix the coverage harness / install". A
# non-empty .bin proves the child executed under the debugger, so its non-zero RC is a test
# exit; a missing/empty .bin means the tool never produced data -> tooling failure.
# Returns 0 on success (rc 0), 1 on a test-binary failure, 2 on a tooling failure.
classify_capture_failure() {
    local label="$1" rc="$2" bin="$3"
    if [ "$rc" -eq 0 ]; then return 0; fi
    if [ -s "$bin" ]; then
        echo "FAIL($label): the test binary returned non-zero exit $rc (real test failure — the" >&2
        echo "  coverage capture itself succeeded; .bin written). Fix the failing $label test(s)." >&2
        return 1
    fi
    echo "FAIL($label): OpenCppCoverage tooling failure (exit $rc, no coverage .bin produced — the" >&2
    echo "  tool could not attach / capture). Check the OpenCppCoverage install + the $label exe path." >&2
    return 2
}

# run_capture <label> <bin> <child-exe> [child-args...]
# One OpenCppCoverage capture of <child-exe> into the <bin> intermediate (reads the globals
# OCC + OCC_FILTER_ARGS). A TOOLING failure (classify verdict 2 — no .bin) is retried ONCE:
# an instrumentation crash is usually transient, and one re-attach is far cheaper than a
# human re-running the whole lane. A TEST-binary failure (verdict 1) is never retried — the
# child ran and failed, so a second run could only hide a flake or repeat the red.
# Returns the final attempt's verdict: 0 ok · 1 test failure · 2 persistent tooling failure.
run_capture() {
    local label="$1" bin="$2" attempt rc verdict
    shift 2
    for attempt in 1 2; do
        rm -f "$bin"
        rc=0
        "$OCC" "${OCC_FILTER_ARGS[@]}" --export_type "binary:$bin" -- "$@" || rc=$?
        verdict=0
        classify_capture_failure "$label" "$rc" "$bin" || verdict=$?
        if [ "$verdict" -eq 2 ] && [ "$attempt" -eq 1 ]; then
            echo "[coverage] $label: tooling failure — retrying the capture once..." >&2
            continue
        fi
        return "$verdict"
    done
}

# coverage_infra_error <detail> — the distinct marker for a coverage-HARNESS failure (backlog
# infra.md 2026-06-14 ci-infra-flake-reds-masquerade-as-real-breakage: an OpenCppCoverage crash
# used to surface as `0% - 1 hit, 544 misses`, indistinguishable from a real threshold miss).
# GitHub Actions renders the line as an error annotation titled COVERAGE-INFRA-CRASH; callers
# exit 3 so the verdict also differs from the test/threshold exit 1.
coverage_infra_error() {
    echo "::error title=COVERAGE-INFRA-CRASH::$1 — a coverage-harness failure, not a test or coverage regression. Re-run the job; if it persists, check the OpenCppCoverage install."
}

# --selftest — exercise classify_capture_failure with synthetic inputs (no build/exe needed).
# selftest: asserts-failure — feeds known-bad (non-zero) RCs and asserts the test-vs-tooling
# split returns the distinct exit codes (1 = test failure with .bin present, 2 = tooling failure
# with no .bin). Kept BEFORE the cd / build-dir resolution so it runs in any CWD.
if [ "${1:-}" = "--selftest" ]; then
    st_tmp="$(mktemp -d)" || { echo "coverage.sh selftest: mktemp failed" >&2; exit 2; }
    trap 'rm -rf "$st_tmp"' EXIT
    st_fail=0
    st_rc=0
    # `set -e` would abort on a non-zero return, so disable it around the probes.
    set +e
    # RC 0 -> success regardless of bin.
    classify_capture_failure "X" 0 "$st_tmp/absent.bin" 2>/dev/null; st_rc=$?
    [ "$st_rc" -eq 0 ] || { echo "selftest FAIL: rc0 not success (got $st_rc)" >&2; st_fail=1; }
    # Non-zero RC with a non-empty .bin -> TEST-binary failure (return 1).
    printf 'data' > "$st_tmp/present.bin"
    classify_capture_failure "X" 5 "$st_tmp/present.bin" 2>/dev/null; st_rc=$?
    [ "$st_rc" -eq 1 ] || { echo "selftest FAIL: test-failure not classified as 1 (got $st_rc)" >&2; st_fail=1; }
    # Non-zero RC with NO .bin -> TOOLING failure (return 2).
    classify_capture_failure "X" 5 "$st_tmp/absent.bin" 2>/dev/null; st_rc=$?
    [ "$st_rc" -eq 2 ] || { echo "selftest FAIL: tooling-failure not classified as 2 (got $st_rc)" >&2; st_fail=1; }
    # Non-zero RC with an EMPTY .bin -> tooling failure (return 2) — empty file is no data.
    : > "$st_tmp/empty.bin"
    classify_capture_failure "X" 5 "$st_tmp/empty.bin" 2>/dev/null; st_rc=$?
    [ "$st_rc" -eq 2 ] || { echo "selftest FAIL: empty-bin not classified as tooling (2) (got $st_rc)" >&2; st_fail=1; }
    # run_capture retry policy, against a stub OpenCppCoverage whose per-invocation behaviour
    # is scripted by STUB_PLAN (one word per call: crash = exit 1 with no .bin · test = .bin
    # written + exit 7 · ok = .bin written + exit 0); STUB_COUNTER counts the invocations.
    cat > "$st_tmp/occ-stub" <<'STUB'
#!/usr/bin/env bash
n=$(( $(cat "$STUB_COUNTER") + 1 )); echo "$n" > "$STUB_COUNTER"
read -r -a plan <<< "$STUB_PLAN"; step="${plan[$((n - 1))]:-ok}"
bin=""; prev=""
for a in "$@"; do
    if [ "$prev" = "--export_type" ] && [ "${a#binary:}" != "$a" ]; then bin="${a#binary:}"; fi
    prev="$a"
done
case "$step" in
    crash) exit 1 ;;
    test) printf 'cov' > "$bin"; exit 7 ;;
    *) printf 'cov' > "$bin"; exit 0 ;;
esac
STUB
    chmod +x "$st_tmp/occ-stub"
    OCC="$st_tmp/occ-stub"
    OCC_FILTER_ARGS=(--sources selftest)
    export STUB_PLAN STUB_COUNTER="$st_tmp/count"
    # "<plan>|<want verdict>|<want invocations>|<why>"
    for st_case in "ok|0|1|clean capture" "crash ok|0|2|transient tooling crash rescued by the retry" \
                   "crash crash|2|2|persistent tooling crash -> infra" "test|1|1|real test failure is never retried"; do
        IFS='|' read -r STUB_PLAN st_want st_calls st_why <<< "$st_case"
        echo 0 > "$STUB_COUNTER"
        st_rc=0
        run_capture "X" "$st_tmp/cap.bin" "child.exe" >/dev/null 2>&1 || st_rc=$?
        st_n="$(cat "$STUB_COUNTER")"
        if [ "$st_rc" -ne "$st_want" ] || [ "$st_n" -ne "$st_calls" ]; then
            echo "selftest FAIL: run_capture '$STUB_PLAN' ($st_why): verdict $st_rc/$st_want, invocations $st_n/$st_calls" >&2
            st_fail=1
        fi
    done
    # The annotation title is what separates an infra red from a coverage red at the rollup.
    case "$(coverage_infra_error "selftest")" in
        "::error title=COVERAGE-INFRA-CRASH::selftest"*) ;;
        *) echo "selftest FAIL: coverage_infra_error does not emit the COVERAGE-INFRA-CRASH annotation" >&2; st_fail=1 ;;
    esac
    set -e
    if [ "$st_fail" -eq 0 ]; then
        echo "coverage.sh --selftest: PASS — test-binary vs OpenCppCoverage tooling exit split OK; tooling crash retried once, test failure never"
        exit 0
    fi
    echo "coverage.sh --selftest: FAIL"
    exit 1
fi

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
            sed -n '2,35p' "$0"
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
# OpenCppCoverage --sources / --modules / --excluded_sources match by
# **substring "contains"** (NOT regex), with `*` as the only wildcard (matches
# any chars incl. path separators). A literal `.` therefore matches ONLY a
# literal dot — so the previous dotted patterns (`Source.Core`,
# `Source.Plugins.Mcp.imgui`, `Source.Core.src.Ui`) never matched the
# backslash Windows paths (`...\Source\Core\src\...`) and silently selected
# ZERO files → empty 0-line coverage report (issue #833). Use `*` between path
# segments so the pattern is separator-agnostic. Plain substrings already
# present in the path (`_deps`, `tests`, `ImGui`, `imgui`) are left as-is.
SOURCE_INCLUDE="Source*Core"
MODULE_INCLUDE="Smatchet"  # matches both SmatchetTests.exe and SmatchetLuaTests.exe

# Quarantined doctest cases (tagged with a `[quarantined:...]` subtag) are known-flaky
# and excluded from the authoritative run elsewhere; they must not flake the coverage
# capture either. doctest's --test-case-exclude takes a wildcard against the case NAME;
# the bracketed-tag convention surfaces in the name as `[quarantined:...]`, so the
# `*[quarantined:*]*` glob drops every quarantined case. Passed AFTER the `--` so it goes
# to the doctest child, not to OpenCppCoverage.
DOCTEST_EXCLUDE_QUARANTINED='--test-case-exclude=*[quarantined:*]*'

OCC_FILTER_ARGS=(
    --sources "$SOURCE_INCLUDE"
    --modules "$MODULE_INCLUDE"
    --excluded_sources "_deps"
    --excluded_sources "tests"
    --excluded_sources "Source*Plugins*Mcp*imgui"
    --excluded_sources "ImGui"
    --excluded_sources "imgui"
    # coverage-threshold-graduation Slice 1: the end-state spec measures
    # Source/Core/src/ EXCLUDING UI draw code (bucket-E/screenshot-tested, not
    # ctest-testable). `*` form matches `Source\Core\src\Ui\` /
    # `Source\Core\include\Ui\` regardless of separator.
    --excluded_sources "Source*Core*src*Ui"
    --excluded_sources "Source*Core*include*Ui"
)

# Capture each target into its own binary intermediate, then merge both via a
# third invocation that reads --input_coverage and exports the final Cobertura
# XML + optional HTML. This is OpenCppCoverage's only merge surface — without
# it, the second --export_type cobertura overwrites the first.
# --no-breaks: OpenCppCoverage attaches via the debug API, so doctest's
# isDebuggerActive() is TRUE under it — and doctest breaks into the debugger on a
# FAILING assertion (incl. a WARN-level one) when no debugger interactively
# handles the break, terminating the child with STATUS_BREAKPOINT (0x80000003 /
# OpenCppCoverage "error code: -2147483645"), which fails the coverage capture
# even though every test PASSED. --no-breaks disables the break; the doctest exit
# code still reflects real failures, so a genuine test failure still fails here.
# (Surfaced by the flaky-quarantine self-test's intentional WARN-on-false.)
echo "[coverage] capturing SmatchetTests via $OCC..."
V_TESTS=0
run_capture "SmatchetTests" "$BIN_TESTS" "$TEST_EXE" --no-intro --no-version --no-breaks "$DOCTEST_EXCLUDE_QUARANTINED" || V_TESTS=$?

echo "[coverage] capturing SmatchetLuaTests..."
V_LUA=0
run_capture "SmatchetLuaTests" "$BIN_LUA" "$LUA_TEST_EXE" --no-intro --no-version --no-breaks "$DOCTEST_EXCLUDE_QUARANTINED" || V_LUA=$?

# Distinguish a TEST-BINARY failure (the doctest child returned non-zero — a real test
# regression OpenCppCoverage faithfully forwarded; the .bin intermediate WAS written) from
# an OpenCppCoverage TOOLING failure (the tool could not attach / produced no coverage data
# even after run_capture's one retry). The two demand different operator action: a test
# failure means "fix the failing test" (exit 1); a tooling failure means "re-run / fix the
# coverage harness" (exit 3, COVERAGE-INFRA-CRASH). A test failure dominates when both occur
# (a re-run cannot clear it), but the infra half is still annotated.
CAPTURE_INFRA=""
if [ "$V_TESTS" -eq 2 ]; then CAPTURE_INFRA="SmatchetTests"; fi
if [ "$V_LUA" -eq 2 ]; then CAPTURE_INFRA="${CAPTURE_INFRA:+$CAPTURE_INFRA + }SmatchetLuaTests"; fi
if [ -n "$CAPTURE_INFRA" ]; then
    coverage_infra_error "OpenCppCoverage produced no coverage data for $CAPTURE_INFRA (capture retried once)"
fi
if [ "$V_TESTS" -eq 1 ] || [ "$V_LUA" -eq 1 ]; then
    exit 1
fi
if [ -n "$CAPTURE_INFRA" ]; then
    exit 3
fi

# xml_line_rate <xml> — print the Cobertura root line-rate (a float in [0,1]), or NOTHING
# when the report is missing / empty / has no parseable root rate. Nothing means the export
# carried no coverage data — an infra failure, never a 0% reading (the old fallback printed
# "0", turning an empty export into a `0% < threshold` red). The path goes via os.environ
# (NOT string interpolation into `-c` source) so it cannot break the Python source or run
# attacker-controlled code under set -euo pipefail.
xml_line_rate() {
    [ -s "$1" ] || return 0
    XML_OUT="$1" python -c '
import os, re
with open(os.environ["XML_OUT"], encoding="utf-8", errors="replace") as f:
    t = f.read()
m = re.search(r"<coverage[^>]*line-rate=\"([0-9.]+)\"", t)
try:
    r = float(m.group(1)) if m else -1.0
except ValueError:
    r = -1.0
print(m.group(1) if 0.0 <= r <= 1.0 else "")
'
}

# Merge the two binaries into the final Cobertura (+ optional HTML) report.
# OpenCppCoverage requires at least one runnable child even on a pure merge; we
# attach a no-op carrier so the tool runs without re-executing either test exe a
# third time. The carrier must be an EXTERNAL exe, not a cmd.exe builtin:
# OpenCppCoverage quotes every child argument, so `cmd.exe /c exit 0` reaches
# cmd as `/c "exit" "0"` and dies with `'"exit"' is not recognized`. `where.exe`
# takes a plain filename argument, is always in System32, and exits 0 on a hit.
# MSYS_NO_PATHCONV is not needed here (no `/switch` argument survives to argv),
# but see tests/bats/msys_argv_switches.bats for the class this used to hit.
# A merge that fails or writes no usable XML is a tooling failure like a capture
# crash: retried once, then reported as COVERAGE-INFRA-CRASH (exit 3).
MERGE_EXPORTS=(--export_type "cobertura:$XML_OUT")
if [ "$XML_ONLY" -eq 0 ]; then
    MERGE_EXPORTS+=(--export_type "html:$HTML_OUT")
fi
LINE_RATE=""
RC_MERGE=0
for MERGE_ATTEMPT in 1 2; do
    echo "[coverage] merging binaries -> $XML_OUT..."
    rm -f "$XML_OUT"
    RC_MERGE=0
    "$OCC" --input_coverage "$BIN_TESTS" --input_coverage "$BIN_LUA" "${MERGE_EXPORTS[@]}" -- where.exe cmd.exe || RC_MERGE=$?
    if [ "$RC_MERGE" -eq 0 ]; then
        LINE_RATE="$(xml_line_rate "$XML_OUT")"
    fi
    if [ -n "$LINE_RATE" ]; then
        break
    fi
    if [ "$MERGE_ATTEMPT" -eq 1 ]; then
        echo "[coverage] merge produced no usable coverage.xml (exit $RC_MERGE) — retrying once..." >&2
    fi
done
if [ -z "$LINE_RATE" ]; then
    if [ "$RC_MERGE" -ne 0 ]; then
        coverage_infra_error "OpenCppCoverage merge returned $RC_MERGE (retried once)"
    else
        coverage_infra_error "$XML_OUT missing, empty or without a coverage line-rate after the merge (retried once)"
    fi
    exit 3
fi

# Cobertura's <coverage line-rate="0.72" .../> is a float in [0,1]; surface a
# percentage for the threshold compare.
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
