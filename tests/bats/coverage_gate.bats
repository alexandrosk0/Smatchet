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

# ---------- coverage-delta-gate.sh: TEST_CHANGES recognition ----------
# The gate cds to its own ../.. — copying it into a fixture repo makes that the
# fixture root, so the whole diff pipeline runs hermetically against fixture git.

# make_fixture_repo — a base commit with one real prod TU, on branch main.
make_fixture_repo() {
    FIXREPO="$(mktemp -d)"
    git -C "$FIXREPO" init -q -b main
    git -C "$FIXREPO" config user.email t@t && git -C "$FIXREPO" config user.name t
    mkdir -p "$FIXREPO/scripts/dev" "$FIXREPO/Source/Core/src"
    cp "$DELTA_GATE" "$FIXREPO/scripts/dev/"
    printf 'int foo() { return 1; }\n' > "$FIXREPO/Source/Core/src/a.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm base
    git -C "$FIXREPO" checkout -qb head
    printf 'int foo() { return 2; }\nint bar() { return 3; }\n' > "$FIXREPO/Source/Core/src/a.cpp"
}

@test "coverage-delta-gate.sh: NEW test dir's *.test.cpp earns gate credit (tests/monkey)" {
    make_fixture_repo
    mkdir -p "$FIXREPO/tests/monkey"
    printf 'int t() { return 0; }\n' > "$FIXREPO/tests/monkey/m.test.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm head
    run env SMATCHET_COVERAGE_GATE_BASE=main bash "$FIXREPO/scripts/dev/coverage-delta-gate.sh"
    rm -rf "$FIXREPO"
    [ "$status" -eq 0 ]
    [[ "$output" == *"production + test files both changed"* ]]
}

@test "coverage-delta-gate.sh: tests/support *.test.cpp earns NO credit (dismissable helper)" {
    make_fixture_repo
    mkdir -p "$FIXREPO/tests/support"
    printf 'int t() { return 0; }\n' > "$FIXREPO/tests/support/h.test.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm head
    run env SMATCHET_COVERAGE_GATE_BASE=main bash "$FIXREPO/scripts/dev/coverage-delta-gate.sh"
    rm -rf "$FIXREPO"
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL"* ]]
}
