#!/usr/bin/env bash
# scripts/dev/local/test-build-wrapper.sh — smoke tests for build.sh,
# build-and-run.sh, build-standalone.sh and run-standalone.sh.
#
# Bash port of the retired test-build-wrapper.ps1, assertion for assertion. No
# real CMake build is triggered: the tests use fake exes and stub delegates, so
# they run on any machine (and in CI) with no compiler installed.
#
# Tests covered:
#   1. build-standalone.sh rejects the retired ninja-iter-msys2 preset with a
#      migration hint.
#   2. run-standalone.sh prints the exe path + timestamp for the selected exe.
#   3. The stale-sibling table is formatted correctly (>=2 existing siblings).
#   4. build.sh preset auto-detect table — one row per compiler-availability
#      branch, asserting both the printed preset and the routing reason.
#   5. An explicit --preset wins over auto-detect and every flag forwards intact.
#   6. cl.exe already on PATH bypasses with-msvc-env.sh.
#   7. with-msvc-env.sh's own toolchain-missing failure (out-of-band sentinel +
#      exit 78) surfaces install hints — and a wrapped command's propagated exit
#      code never does, not even a wrapped 78 with no sentinel.
#   8. build-standalone.sh fails fast on every msvc/clang preset with no cl.exe,
#      before the cmake configure step (including mid-name-token presets).
#   9. The pre-flight does not over-reach: presets with no msvc/clang token
#      never demand cl.exe.
#  10. A clang preset with cl.exe but no clang-cl.exe still fails fast.
#
# Exit codes: 0 = all passed, 1 = one or more failed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

VERBOSE=0
[ "${1:-}" = "--verbose" ] && VERBOSE=1

PASSED=0
FAILED=0
RESULTS=()

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/smatchet-buildwrap.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

# Each test body runs in a subshell and signals failure by printing a reason to
# stderr and returning non-zero, so one failing case never aborts the suite.
run_test() {
    local name="$1" body="$2" detail rc
    detail="$( "$body" 2>&1 )"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        RESULTS+=("PASS|$name|")
        PASSED=$((PASSED + 1))
        [ "$VERBOSE" -eq 1 ] && [ -n "$detail" ] && printf '%s\n' "$detail"
    else
        RESULTS+=("FAIL|$name|$(printf '%s' "$detail" | tr '\n' ' ')")
        FAILED=$((FAILED + 1))
    fi
    return 0
}

fail() { printf '%s\n' "$*" >&2; return 1; }

# A minimal executable stand-in. Named without .exe so `command -v` resolves it
# on the sandbox PATH the same way a real tool would.
make_fake_tool() {
    printf '#!/bin/sh\nexit 0\n' > "$1"
    chmod +x "$1"
}

# ---------------------------------------------------------------------------
# Test 1: retired msys2 preset exits non-zero with a migration hint.
# ---------------------------------------------------------------------------
test_msys2_rejected() {
    local out rc
    out="$(bash "$SCRIPT_DIR/build-standalone.sh" --preset ninja-iter-msys2 2>&1)"
    rc=$?
    [ "$rc" -ne 0 ] || fail "Expected non-zero exit for the retired msys2 preset, got 0." || return 1
    case "$out" in *retired*) ;; *) fail "Expected 'retired' in output. Got: $out"; return 1 ;; esac
    case "$out" in *ninja-iter-msvc*) ;; *) fail "Expected the ninja-iter-msvc migration hint. Got: $out"; return 1 ;; esac
}

# ---------------------------------------------------------------------------
# Test 2: run-standalone.sh prints the exe path + mtime.
# ---------------------------------------------------------------------------
test_run_standalone_prints_exe() {
    local dir out
    dir="$(mktemp -d "$TMP_ROOT/run.XXXXXX")"
    make_fake_tool "$dir/Smatchet.exe"

    # A trailing app arg forces the foreground path, so the script returns at
    # once. Path + timestamp are printed before launch either way.
    out="$(bash "$SCRIPT_DIR/run-standalone.sh" --build-dir "$dir" -- --version 2>&1)"

    case "$out" in *"$dir/Smatchet.exe"*) ;; *) fail "Expected the exe path in output. Got: $out"; return 1 ;; esac
    printf '%s' "$out" | grep -Eq 'Time[[:space:]]*:' || { fail "Expected a 'Time :' line. Got: $out"; return 1; }
}

# ---------------------------------------------------------------------------
# Test 3: the sibling table appears when >=2 sibling exes exist.
# ---------------------------------------------------------------------------
test_run_standalone_sibling_table() {
    local dir out created_files=() created_dirs=() preset sib_dir sib_exe rc=0
    for preset in ninja-debug-msvc ninja-iter-msvc; do
        sib_dir="$REPO_ROOT/build/$preset"
        if [ ! -d "$sib_dir" ]; then
            mkdir -p "$sib_dir"
            created_dirs+=("$sib_dir")
        fi
        sib_exe="$sib_dir/Smatchet.exe"
        if [ ! -f "$sib_exe" ]; then
            make_fake_tool "$sib_exe"
            created_files+=("$sib_exe")
        fi
    done

    # The selected exe lives outside build/, so it is an extra row on top of the
    # two siblings.
    dir="$(mktemp -d "$TMP_ROOT/sib.XXXXXX")"
    make_fake_tool "$dir/Smatchet.exe"

    out="$(bash "$SCRIPT_DIR/run-standalone.sh" --build-dir "$dir" -- --version 2>&1)"

    case "$out" in *"Sibling exe comparison"*) ;; *) fail "Expected the 'Sibling exe comparison' header. Got: $out"; rc=1 ;; esac
    case "$out" in *"<<< selected"*) ;; *) fail "Expected the '<<< selected' marker. Got: $out"; rc=1 ;; esac

    # Clean up only what this test created; files first so the dirs are empty.
    local f d
    for f in ${created_files+"${created_files[@]}"}; do rm -f "$f"; done
    for d in ${created_dirs+"${created_dirs[@]}"}; do rmdir "$d" 2>/dev/null || true; done
    return "$rc"
}

# ---------------------------------------------------------------------------
# Sandbox helpers for the build.sh dispatcher tests (4-7).
#
# build.sh resolves both delegates relative to its own directory, so a sandbox
# holding a copy of build.sh plus stub delegates exercises every dispatch branch
# with no compiler, no cmake configure and no real vcvars import. Which branch
# runs is decided purely by what the child finds on PATH.
# ---------------------------------------------------------------------------
new_sandbox() {
    local with_cl="$1" with_clang_cl="$2" sandbox
    sandbox="$(mktemp -d "$TMP_ROOT/sandbox.XXXXXX")"
    mkdir -p "$sandbox/scripts/dev/local" "$sandbox/bin"
    cp "$REPO_ROOT/build.sh" "$sandbox/build.sh"

    # Echoes what actually bound, so a forwarding regression shows up as a wrong
    # value rather than a silent pass.
    cat > "$sandbox/scripts/dev/local/build-and-run.sh" <<'STUB'
#!/usr/bin/env bash
preset=""; target=""; build_only=False; run_only=False; verify=False; app_args=""
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="$2"; shift 2 ;;
        --target) target="$2"; shift 2 ;;
        --build-only) build_only=True; shift ;;
        --run-only) run_only=True; shift ;;
        --verify) verify=True; shift ;;
        --) shift; app_args="$*"; break ;;
        *) shift ;;
    esac
done
echo "STUB build-and-run: Preset=$preset Target=$target BuildOnly=$build_only RunOnly=$run_only Verify=$verify StandaloneArgs=$app_args"
exit 0
STUB

    # Mirrors the real wrapper's two channels: the exit code (always) and the
    # out-of-band toolchain-missing sentinel (only when the simulated failure is
    # the wrapper's own — SMATCHET_TEST_WITHMSVC_SENTINEL=1). Exit-without-
    # sentinel simulates a WRAPPED command that returned the same code.
    cat > "$sandbox/scripts/dev/with-msvc-env.sh" <<'STUB'
#!/usr/bin/env bash
echo "STUB with-msvc: $*"
if [ -n "${SMATCHET_TEST_WITHMSVC_EXIT:-}" ]; then
    if [ "${SMATCHET_TEST_WITHMSVC_SENTINEL:-0}" = "1" ] && [ -n "${SMATCHET_MSVC_STATUS_FILE:-}" ]; then
        printf 'toolchain-missing\n' > "$SMATCHET_MSVC_STATUS_FILE"
    fi
    exit "$SMATCHET_TEST_WITHMSVC_EXIT"
fi
exec "$@"
STUB

    [ "$with_cl" = "1" ]       && make_fake_tool "$sandbox/bin/cl"
    [ "$with_clang_cl" = "1" ] && make_fake_tool "$sandbox/bin/clang-cl"
    printf '%s\n' "$sandbox"
}

# Runs the sandboxed build.sh; prints its output, returns its exit code.
# The child PATH holds only the sandbox bin plus the shell's own tool dirs, so
# the sandbox alone decides which compiler the dispatcher can see. The third
# argument sets the stub's sentinel behaviour ("1" = the simulated failure is
# the wrapper's own; anything else = a plain wrapped-command exit code).
invoke_sandbox() {
    local sandbox="$1" with_msvc_exit="$2" with_msvc_sentinel="$3"; shift 3
    env PATH="$sandbox/bin:/usr/bin:/bin" \
        SMATCHET_TEST_WITHMSVC_EXIT="$with_msvc_exit" \
        SMATCHET_TEST_WITHMSVC_SENTINEL="$with_msvc_sentinel" \
        bash "$sandbox/build.sh" "$@" 2>&1
}

# ---------------------------------------------------------------------------
# Test 4: preset auto-detect table, one row per compiler-availability branch.
# ---------------------------------------------------------------------------
test_autodetect_table() {
    local row name cl clangcl preset reason sandbox out
    for row in \
        "cl.exe present|1|0|ninja-iter-msvc|cl.exe already on PATH" \
        "clang-cl only|0|1|ninja-iter-clang|no cl.exe on PATH" \
        "no compiler on PATH|0|0|ninja-iter-msvc|no cl.exe on PATH"
    do
        IFS='|' read -r name cl clangcl preset reason <<< "$row"
        sandbox="$(new_sandbox "$cl" "$clangcl")"
        out="$(invoke_sandbox "$sandbox" "" "" --build-only)"

        case "$out" in *"preset $preset (auto-detected)"*) ;;
            *) fail "[$name] expected 'preset $preset (auto-detected)'. Got: $out"; return 1 ;; esac
        case "$out" in *"$reason"*) ;;
            *) fail "[$name] expected routing reason '$reason'. Got: $out"; return 1 ;; esac
        case "$out" in *"Preset=$preset "*) ;;
            *) fail "[$name] expected the preset to reach build-and-run.sh. Got: $out"; return 1 ;; esac
    done
}

# ---------------------------------------------------------------------------
# Test 5: explicit --preset wins over auto-detect; every flag forwards intact.
# ---------------------------------------------------------------------------
test_explicit_preset_wins() {
    local sandbox out
    # No cl.exe, so this also proves the flags survive the with-msvc-env hop.
    sandbox="$(new_sandbox 0 0)"
    out="$(invoke_sandbox "$sandbox" "" "" --preset ninja-debug-msvc --build-only -- --version)"

    case "$out" in *"preset ninja-debug-msvc (explicit --preset overrides auto-detect)"*) ;;
        *) fail "Expected the explicit-preset reason line. Got: $out"; return 1 ;; esac
    case "$out" in *auto-detected*) fail "An explicit --preset must not report auto-detection. Got: $out"; return 1 ;; esac
    case "$out" in *"Preset=ninja-debug-msvc Target=SmatchetStandalone BuildOnly=True RunOnly=False Verify=False StandaloneArgs=--version"*) ;;
        *) fail "Expected every forwarded argument to bind. Got: $out"; return 1 ;; esac
}

# ---------------------------------------------------------------------------
# Test 6: cl.exe already on PATH bypasses with-msvc-env.sh.
# ---------------------------------------------------------------------------
test_cl_bypasses_with_msvc() {
    local sandbox out rc
    sandbox="$(new_sandbox 1 0)"
    out="$(invoke_sandbox "$sandbox" "" "" --build-only)"
    rc=$?

    case "$out" in *"STUB build-and-run:"*) ;;
        *) fail "Expected build-and-run.sh to be invoked. Got: $out"; return 1 ;; esac
    case "$out" in *"STUB with-msvc:"*)
        fail "with-msvc-env.sh must be skipped when cl.exe is on PATH. Got: $out"; return 1 ;; esac
    [ "$rc" -eq 0 ] || { fail "Expected exit 0 on the direct path, got $rc."; return 1; }
}

# ---------------------------------------------------------------------------
# Test 7: the wrapper's own toolchain-missing failure surfaces install hints; a
# propagated child exit code never does.
#
# Row 3 is the regression that motivated SMATCHET_MSVC_ENV_FAIL_RC: with-msvc-env
# propagates the wrapped command's exit code, so a build that legitimately exited
# 2 used to be reported as a missing MSVC install. Row 4 is the assertion the
# exit-code-only design could not make (tooling 2026-08-04): the diagnosis keys
# on the out-of-band sentinel, so even a wrapped command that returns 78 itself
# propagates silently — no hints.
# ---------------------------------------------------------------------------
test_exit78_hints() {
    local row name exit_code sentinel args want_hints want_llvm sandbox out rc saw_diag saw_llvm
    while IFS='|' read -r name exit_code sentinel args want_hints want_llvm; do
        [ -n "$name" ] || continue
        sandbox="$(new_sandbox 0 0)"
        # shellcheck disable=SC2086  # $args is a deliberate multi-flag string
        out="$(invoke_sandbox "$sandbox" "$exit_code" "$sentinel" $args)"
        rc=$?

        [ "$rc" -eq "$exit_code" ] || { fail "[$name] expected exit $exit_code to propagate, got $rc. Output: $out"; return 1; }

        saw_diag=0
        case "$out" in *"no usable MSVC toolchain"*) saw_diag=1 ;; esac
        if [ "$want_hints" != "$saw_diag" ]; then
            if [ "$want_hints" = "1" ]; then
                fail "[$name] expected the no-toolchain diagnosis. Got: $out"
            else
                fail "[$name] a propagated child exit code must not be reported as a missing toolchain. Got: $out"
            fi
            return 1
        fi

        if [ "$want_hints" = "1" ]; then
            case "$out" in *"Install Visual Studio Build Tools with the C++ workload"*) ;;
                *) fail "[$name] Build Tools must be stated as mandatory. Got: $out"; return 1 ;; esac
            case "$out" in *"winget install Microsoft.VisualStudio.2022.BuildTools"*) ;;
                *) fail "[$name] expected the Build Tools winget hint. Got: $out"; return 1 ;; esac
        fi

        saw_llvm=0
        case "$out" in *"winget install LLVM.LLVM"*) saw_llvm=1 ;; esac
        if [ "$want_llvm" != "$saw_llvm" ]; then
            if [ "$want_llvm" = "1" ]; then
                fail "[$name] expected the LLVM hint for a clang preset. Got: $out"
            else
                fail "[$name] LLVM cannot substitute for the MSVC environment; the hint must not appear. Got: $out"
            fi
            return 1
        fi
    done <<'ROWS'
no toolchain, msvc preset|78|1|--build-only|1|0
no toolchain, clang preset|78|1|--preset ninja-iter-clang --build-only|1|1
wrapped build exited 2|2|0|--build-only|0|0
wrapped build exited 78, no sentinel|78|0|--build-only|0|0
ROWS
}

# ---------------------------------------------------------------------------
# Fake-bin helper for the build-standalone.sh pre-flight tests (8-10).
# cmake + ninja must resolve so their presence checks pass and the pre-flight is
# what actually fires; none of the fakes are ever executed.
# ---------------------------------------------------------------------------
new_fake_bin() {
    local dir tool
    dir="$(mktemp -d "$TMP_ROOT/fakebin.XXXXXX")"
    for tool in "$@"; do
        make_fake_tool "$dir/$tool"
    done
    printf '%s\n' "$dir"
}

# ---------------------------------------------------------------------------
# Test 8: every msvc/clang preset without cl.exe fails fast before cmake.
#
# The table is not just ninja-iter-msvc: the toolchain token sits mid-name in the
# sanitizer and arm64 presets, which a "*-msvc" suffix test silently skipped.
# ---------------------------------------------------------------------------
test_preflight_msvc_presets() {
    local fake_bin preset out rc
    fake_bin="$(new_fake_bin cmake ninja)"
    for preset in ninja-iter-msvc ninja-msvc-asan ninja-clang-asan ninja-iter-msvc-arm64 ninja-publish-msvc-arm64; do
        out="$(env PATH="$fake_bin:/usr/bin:/bin" bash "$SCRIPT_DIR/build-standalone.sh" --preset "$preset" 2>&1)"
        rc=$?
        [ "$rc" -ne 0 ] || { fail "[$preset] expected non-zero exit with no cl.exe, got 0. Output: $out"; return 1; }
        case "$out" in *"cl.exe is not on PATH"*) ;;
            *) fail "[$preset] expected the missing-cl.exe diagnosis. Got: $out"; return 1 ;; esac
        case "$out" in *build.sh*) ;;
            *) fail "[$preset] expected the message to name build.sh. Got: $out"; return 1 ;; esac
        case "$out" in *"cmake --preset"*)
            fail "[$preset] the pre-flight must fire before the cmake configure step. Got: $out"; return 1 ;; esac
    done
}

# ---------------------------------------------------------------------------
# Test 9: presets with no msvc/clang token never demand cl.exe.
# ---------------------------------------------------------------------------
test_preflight_no_overreach() {
    local fake_bin preset out
    fake_bin="$(new_fake_bin cmake ninja)"
    for preset in ninja-tsan-linux ninja-test-linux; do
        out="$(env PATH="$fake_bin:/usr/bin:/bin" bash "$SCRIPT_DIR/build-standalone.sh" --preset "$preset" 2>&1)"
        case "$out" in *"cl.exe is not on PATH"*)
            fail "[$preset] carries no msvc/clang token and must not trip the pre-flight. Got: $out"; return 1 ;; esac
    done
}

# ---------------------------------------------------------------------------
# Test 10: a clang preset with cl.exe but no clang-cl.exe still fails fast.
#
# cl.exe is present on purpose: it clears the MSVC-environment gate, so the
# clang-cl gate is the only thing left that can fire.
# ---------------------------------------------------------------------------
test_preflight_clang_cl_missing() {
    local fake_bin preset out rc
    fake_bin="$(new_fake_bin cmake ninja cl)"
    for preset in ninja-iter-clang ninja-clang-asan; do
        out="$(env PATH="$fake_bin:/usr/bin:/bin" bash "$SCRIPT_DIR/build-standalone.sh" --preset "$preset" 2>&1)"
        rc=$?
        [ "$rc" -ne 0 ] || { fail "[$preset] expected non-zero exit with no clang-cl.exe, got 0. Output: $out"; return 1; }
        case "$out" in *"clang-cl.exe is not on PATH"*) ;;
            *) fail "[$preset] expected the missing-clang-cl diagnosis. Got: $out"; return 1 ;; esac
        # The clang-cl message contains the cl.exe message as a substring, so
        # strip it before checking that the earlier gate stayed quiet.
        if printf '%s' "$out" | sed 's/clang-cl\.exe is not on PATH//g' | grep -q "cl\.exe is not on PATH"; then
            fail "[$preset] cl.exe IS on PATH here; the earlier gate must not fire. Got: $out"
            return 1
        fi
        case "$out" in *"cmake --preset"*)
            fail "[$preset] the pre-flight must fire before the cmake configure step. Got: $out"; return 1 ;; esac
    done
}

run_test "build-standalone: ninja-iter-msys2 rejected with migration hint" test_msys2_rejected
run_test "run-standalone: prints exe path and mtime"                      test_run_standalone_prints_exe
run_test "run-standalone: stale sibling comparison table"                 test_run_standalone_sibling_table
run_test "build.sh: preset auto-detect table (cl / clang-cl / neither)"   test_autodetect_table
run_test "build.sh: explicit --preset overrides auto-detect and forwards" test_explicit_preset_wins
run_test "build.sh: cl.exe on PATH bypasses with-msvc-env.sh"             test_cl_bypasses_with_msvc
run_test "build.sh: sentinel surfaces install hints; child exits do not"  test_exit78_hints
run_test "build-standalone: msvc/clang presets fail fast before cmake"    test_preflight_msvc_presets
run_test "build-standalone: non-MSVC presets skip the cl.exe pre-flight"  test_preflight_no_overreach
run_test "build-standalone: clang preset without clang-cl fails fast"     test_preflight_clang_cl_missing

echo ""
echo "Test results:"
for entry in "${RESULTS[@]}"; do
    IFS='|' read -r result name detail <<< "$entry"
    if [ "$result" = "PASS" ]; then
        echo "  [PASS]  $name"
    else
        echo "  [FAIL]  $name"
        [ -n "$detail" ] && echo "          $detail"
    fi
done
echo ""
echo "$PASSED passed, $FAILED failed."

[ "$FAILED" -eq 0 ] || exit 1
exit 0
