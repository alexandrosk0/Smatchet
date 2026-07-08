#!/usr/bin/env bats
# tests/bats/harness_provisioned_doctor.bats — doctor.sh surfaces the
# fresh-clone bootstrap hole (infra 2026-07-06-fresh-clone-bootstrap-hole):
# `[WARN] harness` when .claude/hooks is absent, `[PASS] harness` once the
# HEAD-drift guard is wired. Runs the copied doctor from a temp tree so the
# host's own provisioning state never leaks into the assertion.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    TMP_TREE="$(mktemp -d)"
    mkdir -p "$TMP_TREE/scripts/dev" "$TMP_TREE/agents/scripts/core"
    cp "$REPO_ROOT/scripts/dev/doctor.sh" "$TMP_TREE/scripts/dev/"
    cp "$REPO_ROOT/agents/scripts/core/check-harness-provisioned.sh" \
        "$TMP_TREE/agents/scripts/core/"
}

teardown() {
    rm -rf "$TMP_TREE"
}

@test "doctor warns when .claude/hooks guard is absent (fresh clone)" {
    run bash "$TMP_TREE/scripts/dev/doctor.sh"
    [[ "$output" == *"[WARN] harness"* ]]
    [[ "$output" == *"NOT provisioned"* ]]
    [[ "$output" == *"setup-harness.sh"* ]]
}

@test "doctor passes the harness check when the guard hook is wired" {
    mkdir -p "$TMP_TREE/.claude/hooks"
    : > "$TMP_TREE/.claude/hooks/guard-head-drift.sh"
    run bash "$TMP_TREE/scripts/dev/doctor.sh"
    [[ "$output" == *"[PASS] harness"* ]]
    [[ "$output" != *"[WARN] harness"* ]]
}

@test "doctor skips the harness check when the probe script is missing" {
    rm "$TMP_TREE/agents/scripts/core/check-harness-provisioned.sh"
    run bash "$TMP_TREE/scripts/dev/doctor.sh"
    [[ "$output" != *"harness"* ]]
}
