#!/usr/bin/env bats
# tests/bats/harness_provisioned_doctor.bats — doctor.sh surfaces the
# fresh-clone bootstrap hole (infra 2026-07-06-fresh-clone-bootstrap-hole):
# `[WARN] harness` when .claude/hooks is absent, `[PASS] harness` once the
# HEAD-drift guard is wired. Runs the copied doctor from a temp tree so the
# host's own provisioning state never leaks into the assertion.
#
# The fixture also carries a minimal agent LAYER (one core agent + the shared
# content probe). That is not scenery: the probe now reports an empty layer as
# its own exit 3, so a tree with `agents/scripts/core/` but no agent definitions
# is a genuinely broken tree, and without the layer the two original cases would
# be asserting the guard-hook outcome on a tree the probe rejects for an
# unrelated reason. The layer states get their own cases below.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    TMP_TREE="$(mktemp -d)"
    mkdir -p "$TMP_TREE/scripts/dev" "$TMP_TREE/agents/scripts/core/lib" \
        "$TMP_TREE/agents/core" "$TMP_TREE/agents/project"
    cp "$REPO_ROOT/scripts/dev/doctor.sh" "$TMP_TREE/scripts/dev/"
    cp "$REPO_ROOT/agents/scripts/core/check-harness-provisioned.sh" \
        "$TMP_TREE/agents/scripts/core/"
    cp "$REPO_ROOT/agents/scripts/core/lib/agents-dir-current.sh" \
        "$TMP_TREE/agents/scripts/core/lib/"
    printf 'canonical v1\n' > "$TMP_TREE/agents/core/sample-agent.md"
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

# --- agent-layer states (plan agent-surface-extraction-repo, rows 4c + 5e) ---

@test "doctor warns about the agent layer when it is empty (submodule not initialised)" {
    mkdir -p "$TMP_TREE/.claude/hooks"
    : > "$TMP_TREE/.claude/hooks/guard-head-drift.sh"
    rm -f "$TMP_TREE/agents/core/sample-agent.md"
    run bash "$TMP_TREE/scripts/dev/doctor.sh"
    [[ "$output" == *"[WARN] harness"* ]]
    # The remedy must be the submodule one. Reporting the guard-hook message
    # here would send the reader to setup-harness.sh, which fixes nothing.
    [[ "$output" == *"git submodule update --init --recursive"* ]]
    [[ "$output" != *"NOT provisioned"* ]]
}

@test "doctor warns when .claude/agents content is stale relative to the layer" {
    mkdir -p "$TMP_TREE/.claude/hooks" "$TMP_TREE/.claude/agents"
    : > "$TMP_TREE/.claude/hooks/guard-head-drift.sh"
    # The row 4c state exactly: the link still exists and the count still
    # matches; only the bytes differ, as after a submodule update replaced the
    # canonical file's inode out from under a hardlink.
    printf 'stale v0\n' > "$TMP_TREE/.claude/agents/sample-agent.md"
    run bash "$TMP_TREE/scripts/dev/doctor.sh"
    [[ "$output" == *"[WARN] harness"* ]]
    [[ "$output" == *"STALE"* ]]
}

@test "doctor passes when the layer is populated and .claude/agents matches it" {
    mkdir -p "$TMP_TREE/.claude/hooks" "$TMP_TREE/.claude/agents"
    : > "$TMP_TREE/.claude/hooks/guard-head-drift.sh"
    cp "$TMP_TREE/agents/core/sample-agent.md" "$TMP_TREE/.claude/agents/sample-agent.md"
    run bash "$TMP_TREE/scripts/dev/doctor.sh"
    [[ "$output" == *"[PASS] harness"* ]]
    [[ "$output" != *"[WARN] harness"* ]]
}
