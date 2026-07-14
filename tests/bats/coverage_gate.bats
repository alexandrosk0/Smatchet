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

# ---------- coverage.sh: quarantine-exclude wired into BOTH captures ----------
# The quarantine-safe capture fix (ci-falsepositive-hardening Cluster-C /
# coverage-quarantine-safe) passes `--test-case-exclude=*[quarantined:*]*` to the
# doctest child on every capture so a known-flaky quarantined case cannot flake
# the Coverage lane and masquerade as a coverage regression. That flag living in
# the script is the entire fix — if it is silently dropped the false-red returns.
# This pins it: the flag must be defined AND handed to both the SmatchetTests and
# SmatchetLuaTests capture invocations (a full OpenCppCoverage integration run
# needs the Windows toolchain, so this is a static regression-pin, not a capture).
@test "coverage.sh: quarantine-exclude flag defined and passed to both captures" {
    # Defined once as the shared flag var.
    run grep -qF "DOCTEST_EXCLUDE_QUARANTINED='--test-case-exclude=*[quarantined:*]*'" "$COVERAGE"
    [ "$status" -eq 0 ]
    # Handed to the doctest child (after `--`) on exactly the two capture lines —
    # SmatchetTests + SmatchetLuaTests. Two references beyond the definition.
    run bash -c "grep -c '\"\$DOCTEST_EXCLUDE_QUARANTINED\"' '$COVERAGE'"
    [ "$status" -eq 0 ]
    [ "$output" -ge 2 ]
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
