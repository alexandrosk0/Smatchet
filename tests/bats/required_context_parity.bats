#!/usr/bin/env bats
# tests/bats/required_context_parity.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/test-required-context-parity.sh +
# agents/scripts/core/lib/ci-check-resolve.sh.
#
# Drives the gate against synthetic workflow + config fixtures (tests/fixtures/
# ci_parity_*). Covers the four required cases — clean PASSES, path-filtered
# FAILS, unresolvable FAILS, self-gate-marked PASSES — plus the deliberately-
# broken negative-test fixture (build-log-regex lesson: a false-pass regression
# must be caught at authoring time).
#
# Requires: bash, python (with PyYAML on the chosen interpreter), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export GATE="$REPO_ROOT/agents/scripts/core/test-required-context-parity.sh"
    export FIXTURES_DIR="$REPO_ROOT/tests/fixtures"
    # A scratch workflows dir the resolver points at via CI_WORKFLOWS_DIR.
    WF_DIR="$(mktemp -d)"
    export WF_DIR
    cp "$FIXTURES_DIR"/ci_parity_wf_*.yml "$WF_DIR"/
}

teardown() {
    rm -rf "$WF_DIR"
}

# Run the gate's --check against the synthetic config + workflow fixtures.
run_gate_check() {
    CI_PARITY_CONFIG="$FIXTURES_DIR/$1" CI_WORKFLOWS_DIR="$WF_DIR" \
        bash "$GATE" --check
}

@test "selftest passes (fire on path-filtered + unresolvable, clean on the rest)" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "clean: a required context on an unconditional workflow passes" {
    run run_gate_check ci_parity_config_clean.json
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "path-filtered: a required context whose workflow is paths-filtered FAILS" {
    run run_gate_check ci_parity_config_filtered.json
    [ "$status" -eq 1 ]
    [[ "$output" == *"paths:/paths-ignore: filter"* ]]
}

@test "unresolvable: a required context no workflow emits FAILS (fail-closed)" {
    run run_gate_check ci_parity_config_unresolvable.json
    [ "$status" -eq 1 ]
    [[ "$output" == *"UNRESOLVABLE"* ]]
}

@test "self-gated: a path-filtered workflow carrying the marker PASSES" {
    run run_gate_check ci_parity_config_selfgated.json
    [ "$status" -eq 0 ]
    [[ "$output" == *"self-gated"* ]]
}

@test "templated: a matrix-leg required context resolves by literal prefix" {
    run run_gate_check ci_parity_config_templated.json
    [ "$status" -eq 0 ]
    [[ "$output" == *"templated-prefix"* ]]
}

# Negative-test fixture (build-log-regex lesson): a config that SHOULD fail must
# actually fail. If the gate ever regresses to pass-through, this catches it.
@test "negative-test: the path-filtered fixture does NOT silently pass" {
    run run_gate_check ci_parity_config_filtered.json
    [ "$status" -ne 0 ]
}

@test "resolver: ci_resolve_check resolves a clean job and reports no paths filter" {
    run bash -c "source '$REPO_ROOT/agents/scripts/core/lib/ci-check-resolve.sh'; CI_WORKFLOWS_DIR='$WF_DIR' ci_resolve_check 'Clean Context'"
    [ "$status" -eq 0 ]
    # status \t wf \t job \t has_pr_paths_filter \t self_gated \t templated
    [[ "$output" == "RESOLVED"*"false"* ]]
}

@test "resolver: ci_resolve_check fail-closes (rc1) on an unknown name" {
    run bash -c "source '$REPO_ROOT/agents/scripts/core/lib/ci-check-resolve.sh'; CI_WORKFLOWS_DIR='$WF_DIR' ci_resolve_check 'Nope Not Here'"
    [ "$status" -eq 1 ]
    [[ "$output" == "UNRESOLVABLE"* ]]
}
