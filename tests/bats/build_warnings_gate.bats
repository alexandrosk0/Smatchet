#!/usr/bin/env bats
# tests/bats/build_warnings_gate.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/test-build-warnings.sh — the toolchain-aware
# unused-warning matcher.
#
# Regression guard for the format-blind gate (historical-review CRITICAL #86):
# the default ninja-iter-msvc preset builds with cl.exe, whose diagnostics carry
# MSVC codes (warning C4101/C4189/C4505), never GCC's "[-Wunused-..]" tag — so a
# GCC-only grep made the gate permanently report "Passed: 1" under MSVC.
#
# Drives the matcher via SMATCHET_WARN_LOG_OVERRIDE (scan a fixture log, skip the
# build) so no compiler is required.
#
# Requires: bash, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/scripts/dev/test-build-warnings.sh"
    LOGFILE="$BATS_TEST_TMPDIR/warn.log"
}

# Write the given lines to the fixture log and run the gate against it.
run_gate() {
    printf '%s\n' "$@" > "$LOGFILE"
    SMATCHET_WARN_LOG_OVERRIDE="$LOGFILE" run bash "$GATE"
}

@test "MSVC C4101 (unused variable) on owned Core path fails" {
    run_gate "../../Source/Core/src/Tracker/JiraClient.cpp(42): warning C4101: 'x': unreferenced local variable"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Failed: 1"* ]]
}

@test "MSVC C4505 (unused function) on owned Standalone path fails" {
    run_gate "../../Source/Standalone/main.cpp(9): warning C4505: 'helper': unreferenced local function has been removed"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Failed: 1"* ]]
}

@test "MSVC C4189 (unused-but-set) on owned Plugins path fails" {
    run_gate "../../Source/Plugins/Mcp/McpServer.cpp(7): warning C4189: 'tmp': local variable is initialized but not referenced"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Failed: 1"* ]]
}

@test "MSVC warning with backslash path separator still matches owned path" {
    run_gate '..\..\Source\Core\src\Sync\TicketSyncService.cpp(3): warning C4101: '"'"'n'"'"': unreferenced local variable'
    [ "$status" -eq 1 ]
    [[ "$output" == *"Failed: 1"* ]]
}

@test "GCC/Clang [-Wunused-variable] tag on owned path still fails (regression)" {
    run_gate "../../Source/Core/src/Config/ConfigStore.cpp:11:9: warning: unused variable 'q' [-Wunused-variable]"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Failed: 1"* ]]
}

@test "two owned unused warnings report Failed: 2" {
    run_gate \
        "../../Source/Core/src/Tracker/JiraClient.cpp(42): warning C4101: 'x': unreferenced local variable" \
        "../../Source/Core/src/Tracker/PlaneClient.cpp(8): warning C4189: 'y': local variable is initialized but not referenced"
    [ "$status" -eq 1 ]
    [[ "$output" == *"Failed: 2"* ]]
}

@test "MSVC C4101 under _deps/ (upstream) is excluded" {
    run_gate "build/ninja-iter-msvc/_deps/cpr-src/foo.cpp(5): warning C4101: 'z': unreferenced local variable"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 1"* ]]
}

@test "MSVC C4101 on a non-owned path (ThirdParty) is excluded" {
    run_gate "../../ThirdParty/whatever/foo.cpp(5): warning C4101: 'z': unreferenced local variable"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 1"* ]]
}

@test "a non-unused MSVC warning (C4244) on an owned path does not fail" {
    run_gate "../../Source/Core/src/Tracker/JiraClient.cpp(42): warning C4244: 'argument': conversion from 'int' to 'short'"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 1"* ]]
}

@test "clean log passes" {
    run_gate "[120/120] Linking CXX executable SmatchetStandalone.exe"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 1"* ]]
}

@test "missing override log exits 2" {
    SMATCHET_WARN_LOG_OVERRIDE="$BATS_TEST_TMPDIR/does-not-exist.log" run bash "$GATE"
    [ "$status" -eq 2 ]
}
