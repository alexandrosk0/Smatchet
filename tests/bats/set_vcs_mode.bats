#!/usr/bin/env bats
# tests/bats/set_vcs_mode.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/set-vcs-mode.sh — the per-machine VCS-mode setter.
#
# Drives the script through its test hooks (SMATCHET_VCS_RC overrides the rc
# file; SMATCHET_VCS_SKIP_REGISTRY=1 skips the Windows setx side effect) so the
# tests never touch the real ~/.bashrc or the User registry.
#
# Requires: bash, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/scripts/dev/set-vcs-mode.sh"
    RC="$(mktemp)"
    export SMATCHET_VCS_RC="$RC"
    export SMATCHET_VCS_SKIP_REGISTRY=1
}

teardown() {
    rm -f "$RC" "$RC.cmp"
}

run_mode() {
    run bash "$SCRIPT" "$@"
}

@test "git mode writes the managed block with git-ref backend" {
    run_mode git
    [ "$status" -eq 0 ]
    grep -qx '# >>> smatchet vcs-mode >>>' "$RC"
    grep -qx 'export SMATCHET_AGENT_VCS=git' "$RC"
    grep -qx 'export SMATCHET_LOCK_BACKEND=git-ref' "$RC"
    grep -qx '# <<< smatchet vcs-mode <<<' "$RC"
}

@test "p4 mode writes the managed block with p4-counter backend" {
    run_mode p4
    [ "$status" -eq 0 ]
    grep -qx 'export SMATCHET_AGENT_VCS=p4' "$RC"
    grep -qx 'export SMATCHET_LOCK_BACKEND=p4-counter' "$RC"
}

@test "re-running the same mode is byte-identical (idempotent)" {
    run_mode git
    cp "$RC" "$RC.cmp"
    run_mode git
    [ "$status" -eq 0 ]
    diff -q "$RC.cmp" "$RC"
}

@test "toggling git -> p4 replaces the block in place (no duplicate)" {
    run_mode git
    run_mode p4
    [ "$status" -eq 0 ]
    [ "$(grep -c 'smatchet vcs-mode >>>' "$RC")" -eq 1 ]
    [ "$(grep -c '^export SMATCHET_AGENT_VCS=' "$RC")" -eq 1 ]
    grep -qx 'export SMATCHET_AGENT_VCS=p4' "$RC"
}

@test "legacy unmarked exports are stripped, not duplicated" {
    printf '%s\n' \
        '# Smatchet dual-VCS opt-in' \
        'export SMATCHET_AGENT_VCS=p4' \
        'export SMATCHET_LOCK_BACKEND=p4-counter' > "$RC"
    run_mode git
    [ "$status" -eq 0 ]
    # The legacy comment line is gone; only the managed-block exports remain.
    # (run + status, not bare `! grep`: a `!`-inverted command is exempt from
    # bats' errexit, so `! grep` would never fail the test — SC2314.)
    run grep -q '# Smatchet dual-VCS opt-in' "$RC"
    [ "$status" -ne 0 ]
    [ "$(grep -c '^export SMATCHET_AGENT_VCS=' "$RC")" -eq 1 ]
    grep -qx 'export SMATCHET_AGENT_VCS=git' "$RC"
}

@test "unrelated rc content is preserved" {
    # shellcheck disable=SC2016  # literal $PATH on purpose — asserted verbatim below
    printf '%s\n' 'export PATH=$PATH:/foo' 'alias ll="ls -la"' > "$RC"
    run_mode git
    [ "$status" -eq 0 ]
    # shellcheck disable=SC2016  # literal $PATH — matching the verbatim line written above
    grep -qx 'export PATH=$PATH:/foo' "$RC"
    grep -qx 'alias ll="ls -la"' "$RC"
}

@test "no argument prints the current mode and exits 0" {
    SMATCHET_AGENT_VCS=git SMATCHET_LOCK_BACKEND=git-ref run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"SMATCHET_AGENT_VCS=git"* ]]
    [[ "$output" == *"SMATCHET_LOCK_BACKEND=git-ref"* ]]
}

@test "unknown mode exits 2" {
    run_mode banana
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown mode 'banana'"* ]]
}

@test "emitted managed block is ASCII-only (no mojibake risk on round-trip)" {
    run_mode git
    [ "$status" -eq 0 ]
    # Any byte > 0x7F in the rc would corrupt under a cp1252 PowerShell read.
    run grep -nP '[^\x00-\x7F]' "$RC"
    [ "$status" -ne 0 ]
}
