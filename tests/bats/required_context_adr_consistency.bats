#!/usr/bin/env bats
# tests/bats/required_context_adr_consistency.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/test-required-context-adr-consistency.sh —
# the gate that flags a branch_protection.required_contexts ADDITION that an
# ADR / shipped plan explicitly rejected (the ADR-0022 Intent-section /
# plan-lock Q7 class). Drives the gate against a throwaway fixture tree via
# RC_ADR_ROOT + RC_ADR_BASE_CONTEXTS (no git/network).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/agents/scripts/core/test-required-context-adr-consistency.sh"
    FIX="$(mktemp -d)"
    mkdir -p "$FIX/docs/adr" "$FIX/docs/plans/shipped"
    cat > "$FIX/docs/adr/0099-fixture.md" <<'MD'
# ADR-0099 — fixture

`Phantom gate` was EXPLICITLY REJECTED as a required context — do not add
it to branch_protection.required_contexts (label hatches cannot reach it).
MD
}

teardown() {
    rm -rf "$FIX"
}

cfg() { printf '{"branch_protection":{"required_contexts":%s}}\n' "$1" > "$FIX/project.config.json"; }

@test "--selftest passes (asserts the failure case)" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"--selftest: PASS"* ]]
}

@test "added context with a documented rejection -> FAIL with citation" {
    cfg '["Old gate","Phantom gate"]'
    run env RC_ADR_ROOT="$FIX" RC_ADR_BASE_CONTEXTS="Old gate" bash "$GATE" --check
    [ "$status" -eq 1 ]
    [[ "$output" == *"Phantom gate"* ]]
    [[ "$output" == *"0099-fixture.md"* ]]
}

@test "added context with no rejection -> PASS" {
    cfg '["Old gate","Clean gate"]'
    run env RC_ADR_ROOT="$FIX" RC_ADR_BASE_CONTEXTS="Old gate" bash "$GATE" --check
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "rejected name already in the base set -> PASS (only additions checked)" {
    cfg '["Phantom gate"]'
    run env RC_ADR_ROOT="$FIX" RC_ADR_BASE_CONTEXTS="Phantom gate" bash "$GATE" --check
    [ "$status" -eq 0 ]
}

@test "shipped-plan rejection is also in the corpus" {
    # Path split so the plan-ref-integrity scanner never sees a plan literal.
    cat > "$FIX/docs/plans"/shipped/fixture-plan.md <<'MD'
Q7 decided `Queue gate` must NOT be a required context; do not add it.
MD
    cfg '["Queue gate"]'
    run env RC_ADR_ROOT="$FIX" RC_ADR_BASE_CONTEXTS="" bash "$GATE" --check
    [ "$status" -eq 1 ]
    [[ "$output" == *"fixture-plan.md"* ]]
}
