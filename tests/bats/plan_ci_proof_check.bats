#!/usr/bin/env bats
# tests/bats/plan_ci_proof_check.bats
# ----------------------------------------------------------------------------
# scripts/dev/plan-ci-proof-check.sh: a plan whose `## Verification` names a
# delta-gated CI check as its proof on the PR must not ship a diff on which that
# check skips (a skipped check is a pass). Replays #2400 — its plan asked for
# "Sanitizer (UBSan via Clang)" green on this PR, and the lane skipped on it.
#
# Each case builds a throwaway repository (base on `develop`, one branch commit)
# and runs the SUT there with base `develop`. The SUT runs the real
# scripts/ci/detect-changes.sh from this checkout. --live cases put a stub `gh`
# first on PATH; no network.
#
# Requires: bash, git, awk, sed, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SUT="$REPO_ROOT/scripts/dev/plan-ci-proof-check.sh"
    SANDBOX="$(mktemp -d "${BATS_TMPDIR:-/tmp}/plan-proof.XXXXXX")"
    # Sandbox plan paths are built, not literal, so the plan-ref-integrity doc gate
    # does not read the fixtures as references to real plans.
    FIXTURE_PLAN_DIR="docs/plans/""active"
    STUB_BIN="$SANDBOX/.stub-bin"
    git -C "$SANDBOX" init -q -b develop
    git -C "$SANDBOX" config user.email bats@example.invalid
    git -C "$SANDBOX" config user.name bats
    git -C "$SANDBOX" config commit.gpgsign false
    printf '.stub-bin/\n' > "$SANDBOX/.gitignore"
    printf 'seed\n' > "$SANDBOX/README.md"
    git -C "$SANDBOX" add -A
    git -C "$SANDBOX" commit -q -m base
    git -C "$SANDBOX" checkout -q -b feature
}

teardown() {
    [ -n "${SANDBOX:-}" ] && rm -rf "$SANDBOX"
}

# write_plan <verification-line> [<actual-line>] — a plan naming a CI proof.
write_plan() {
    mkdir -p "$SANDBOX/$FIXTURE_PLAN_DIR"
    {
        printf '# Plan\n\n## Verification\n\n- Local: build it.\n%s\n\n' "$1"
        printf '## Verification (actual)\n\n%s\n' "${2:-- nothing yet}"
    } > "$SANDBOX/$FIXTURE_PLAN_DIR/lane-change.md"
}

commit_path() {
    mkdir -p "$SANDBOX/$(dirname "$1")"
    printf 'change\n' >> "$SANDBOX/$1"
    git -C "$SANDBOX" add -A
    git -C "$SANDBOX" commit -q -m "touch $1"
}

commit_all() {
    git -C "$SANDBOX" add -A
    git -C "$SANDBOX" commit -q -m plan
}

# stub_gh <name> <conclusion> — a gh whose `pr view` reports one check.
stub_gh() {
    mkdir -p "$STUB_BIN"
    cat > "$STUB_BIN/gh" <<EOF
#!/usr/bin/env bash
[ "\$1 \$2" = "pr view" ] || exit 1
printf '%s\t%s\n' "$1" "$2"
EOF
    chmod +x "$STUB_BIN/gh"
}

run_sut() {
    run bash -c 'cd "$1" && shift && PATH="$STUB/:$PATH" bash "$@"' _ "$SANDBOX" "$SUT" "$@"
}

@test "replay #2400: the plan names the UBSan lane, the diff skips it -> refuse" {
    write_plan '- CI: "Sanitizer (UBSan via Clang)" green on this PR with the instrumented rig.'
    commit_all
    commit_path Source/Standalone/Main.cpp
    STUB="$STUB_BIN" run_sut develop
    [ "$status" -eq 1 ]
    [[ "$output" == *'Sanitizer (UBSan via Clang)'* ]]
    [[ "$output" == *'source_core_cpp=false'* ]]
}

@test "the same plan with a lane-config change in the diff -> pass" {
    write_plan '- CI: "Sanitizer (UBSan via Clang)" green on this PR.'
    commit_all
    commit_path cmake/Sanitizers.cmake
    STUB="$STUB_BIN" run_sut develop
    [ "$status" -eq 0 ]
    [[ "$output" == *'will run on this PR (source_core_cpp=true)'* ]]
}

@test "backtick-quoted check name and 'the PR' are recognised" {
    write_plan '- CI: `Sanitizer (ASAN via MSVC)` green on the PR.'
    commit_all
    STUB="$STUB_BIN" run_sut develop
    [ "$status" -eq 1 ]
}

@test "an uncommitted plan edit is in scope" {
    # A committed diff to classify; an empty one fails safe to "the lane runs".
    commit_path Source/Standalone/Main.cpp
    write_plan '- CI: "Sanitizer (UBSan via Clang)" green on this PR.'
    STUB="$STUB_BIN" run_sut develop
    [ "$status" -eq 1 ]
}

@test "only ## Verification is read, not ## Verification (actual)" {
    write_plan '- Local: nothing on CI.' '- CI: "Sanitizer (UBSan via Clang)" green on this PR.'
    commit_all
    STUB="$STUB_BIN" run_sut develop
    [ "$status" -eq 0 ]
    [[ "$output" == *'PASS — 0 CI proof'* ]]
}

@test "a check with no known gate is reported, not refused" {
    write_plan '- CI: "Agentic self-tests (bats)" green on this PR.'
    commit_all
    STUB="$STUB_BIN" run_sut develop
    [ "$status" -eq 0 ]
    [[ "$output" == *'not a known delta-gated check'* ]]
}

@test "no changed plan -> nothing to check" {
    commit_path Source/Standalone/Main.cpp
    STUB="$STUB_BIN" run_sut develop
    [ "$status" -eq 0 ]
    [[ "$output" == *'no changed plan doc'* ]]
}

@test "--live: the named check concluded SKIPPED on the PR head -> refuse" {
    write_plan '- CI: "Sanitizer (UBSan via Clang)" green on this PR.'
    commit_all
    commit_path cmake/Sanitizers.cmake
    stub_gh 'Sanitizer (UBSan via Clang)' SKIPPED
    STUB="$STUB_BIN" run_sut --live develop
    [ "$status" -eq 1 ]
    [[ "$output" == *'concluded SKIPPED on the PR head'* ]]
}

@test "--live: the named check succeeded on the PR head -> pass" {
    write_plan '- CI: "Sanitizer (UBSan via Clang)" green on this PR.'
    commit_all
    commit_path cmake/Sanitizers.cmake
    stub_gh 'Sanitizer (UBSan via Clang)' SUCCESS
    STUB="$STUB_BIN" run_sut --live develop
    [ "$status" -eq 0 ]
    [[ "$output" == *'on the PR head: SUCCESS'* ]]
}

@test "--live without a readable PR reports and does not refuse" {
    write_plan '- CI: "Sanitizer (UBSan via Clang)" green on this PR.'
    commit_all
    commit_path cmake/Sanitizers.cmake
    mkdir -p "$STUB_BIN"
    printf '#!/usr/bin/env bash\nexit 1\n' > "$STUB_BIN/gh"
    chmod +x "$STUB_BIN/gh"
    STUB="$STUB_BIN" run_sut --live develop
    [ "$status" -eq 0 ]
    [[ "$output" == *'live check skipped'* ]]
}

@test "unknown option is a usage error" {
    STUB="$STUB_BIN" run_sut --bogus
    [ "$status" -eq 2 ]
}

@test "pre-ship.sh runs the check" {
    grep -qF 'scripts/dev/plan-ci-proof-check.sh' "$REPO_ROOT/scripts/dev/pre-ship.sh"
}
