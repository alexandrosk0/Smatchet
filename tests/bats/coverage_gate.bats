#!/usr/bin/env bats
# tests/bats/coverage_gate.bats
# ----------------------------------------------------------------------------
# Bats coverage for the two coverage-gate scripts' --selftest surfaces:
#   * scripts/dev/coverage.sh            — the test-binary-vs-OpenCppCoverage-tooling
#                                          exit split (quarantine-safe capture fix).
#   * scripts/dev/coverage-delta-gate.sh — the test-light exemption classifier,
#                                          incl. the multi-line wrapped LOG_* join.
#
# These scripts each ship a hermetic `--selftest` (no build dir / exe / network
# needed); this suite just runs them under bats so a regression reds a discoverable
# test-*.sh wrapper (test-all.sh + test-orphan-bats.sh), not only a hand-run flag.
#
# Requires: bash, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export COVERAGE="$REPO_ROOT/scripts/dev/coverage.sh"
    export DELTA_GATE="$REPO_ROOT/scripts/dev/coverage-delta-gate.sh"
}

# ---------- coverage.sh: test-binary vs tooling exit split ----------

@test "coverage.sh --selftest passes (test-binary vs OpenCppCoverage tooling exit split)" {
    run bash "$COVERAGE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"coverage.sh --selftest: PASS"* ]]
}

# ---------- coverage-delta-gate.sh: classifier incl. wrapped LOG_* join ----------

@test "coverage-delta-gate.sh --selftest passes (classifier + multi-line LOG_ join)" {
    run bash "$DELTA_GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"coverage-delta-gate --selftest: PASS"* ]]
    [[ "$output" == *"2-line wrapped LOG_ERROR"* ]]
    [[ "$output" == *"3-line wrapped LOG_ERROR"* ]]
}
