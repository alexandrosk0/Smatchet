#!/usr/bin/env bats
# tests/bats/coverage_perfile_gate.bats
# ----------------------------------------------------------------------------
# Bats wrapper for scripts/dev/coverage-perfile-gate.sh's hermetic --selftest
# (per-file >=90% line-coverage floor on the named high-risk units). The script's
# --selftest synthesises Cobertura XML fixtures (below-floor / at-floor / escaped /
# missing) and asserts the gate's PASS *and* FAIL paths; this suite runs it under
# bats so a regression reds a discoverable test-*.sh wrapper (test-all.sh +
# test-orphan-bats.sh), not only a hand-run flag.
#
# Requires: bash, bats, python.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export PERFILE_GATE="$REPO_ROOT/scripts/dev/coverage-perfile-gate.sh"
}

@test "coverage-perfile-gate.sh --selftest passes (below-floor fails, escape/at-floor/fail-open pass)" {
    run bash "$PERFILE_GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"coverage-perfile-gate --selftest: PASS"* ]]
}

@test "coverage-perfile-gate.sh fails open when the coverage XML is absent" {
    run bash "$PERFILE_GATE" --xml "$BATS_TEST_TMPDIR/no-such.xml"
    [ "$status" -eq 0 ]
    [[ "$output" == *"fail-open"* ]]
}

@test "coverage-perfile-gate.sh rejects a non-integer --floor" {
    run bash "$PERFILE_GATE" --floor bogus
    [ "$status" -eq 2 ]
}
