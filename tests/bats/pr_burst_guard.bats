#!/usr/bin/env bats
# tests/bats/pr_burst_guard.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/pr-burst-guard.sh — the advisory CodeRabbit-quota
# burst guard. Open-PR count is injected via SMATCHET_PR_BURST_COUNT (no gh /
# network). Threshold via SMATCHET_PR_BURST_THRESHOLD.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GUARD="$REPO_ROOT/scripts/dev/pr-burst-guard.sh"
}

@test "under threshold: OK, exit 0" {
    run env SMATCHET_PR_BURST_COUNT=2 SMATCHET_PR_BURST_THRESHOLD=4 bash "$GUARD"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK — 2 open PR"* ]]
}

@test "at threshold: advisory WARN, still exit 0 (non-strict)" {
    run env SMATCHET_PR_BURST_COUNT=4 SMATCHET_PR_BURST_THRESHOLD=4 bash "$GUARD"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]
}

@test "over threshold under --strict: exit 1" {
    run env SMATCHET_PR_BURST_COUNT=9 SMATCHET_PR_BURST_THRESHOLD=4 bash "$GUARD" --strict
    [ "$status" -eq 1 ]
    [[ "$output" == *"WARN"* ]]
}

@test "over threshold without --strict: advisory exit 0" {
    run env SMATCHET_PR_BURST_COUNT=9 SMATCHET_PR_BURST_THRESHOLD=4 bash "$GUARD"
    [ "$status" -eq 0 ]
}

@test "non-numeric count: SKIP, never blocks" {
    run env SMATCHET_PR_BURST_COUNT=oops bash "$GUARD" --strict
    [ "$status" -eq 0 ]
    [[ "$output" == *"SKIP"* ]]
}

@test "--selftest passes" {
    run bash "$GUARD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}
