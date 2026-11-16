#!/usr/bin/env bats
# tests/bats/detect_changes.bats
# ----------------------------------------------------------------------------
# Pins the trigger set of build-and-test.yml's `changes` job
# (scripts/ci/detect-changes.sh), so the sanitizer lanes cannot silently stop
# running on the PRs that reconfigure them.
#
# The defect this guards (infra 2026-11-12-delta-gated-lane-skips-the-pr-that-
# reconfigures-it): #2400 turned on UBSan and the instrumented doctest rig for
# the required "Sanitizer (UBSan via Clang)" check, but changed no Source/Core
# C++, so `source_core_cpp` stayed false, the lane skipped on its own PR, and a
# skipped required check is a pass. Its toolchain crash first surfaced on three
# unrelated PRs.
#
# Each case builds a throwaway repository with a base commit and one branch
# commit, runs the script over `base...HEAD` exactly as the workflow does, and
# reads its stdout. The workflow cases start from a copy of the real
# build-and-test.yml, so the job-range logic is tested against the file it
# guards.
#
# Also pins the wiring: the workflow must call the script, and must not carry a
# second inline copy of the classification that could drift from it. And the
# nightly clang job must build and run the instrumented rig under the pinned
# LLVM, so a lane change every PR skipped still executes on develop daily.
#
# Requires: bash, git, awk, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SUT="$REPO_ROOT/scripts/ci/detect-changes.sh"
    WF="$REPO_ROOT/.github/workflows/build-and-test.yml"
    NIGHTLY="$REPO_ROOT/.github/workflows/sanitizer-nightly.yml"
    SANDBOX="$(mktemp -d "${BATS_TMPDIR:-/tmp}/detect-changes.XXXXXX")"
    # Sandbox plan paths are built, not literal, so the plan-ref-integrity doc gate
    # does not read the fixtures as references to real plans.
    FIXTURE_PLAN_DIR="docs/plans/""active"
    git -C "$SANDBOX" init -q -b develop
    git -C "$SANDBOX" config user.email bats@example.invalid
    git -C "$SANDBOX" config user.name bats
    git -C "$SANDBOX" config commit.gpgsign false
    mkdir -p "$SANDBOX/.github/workflows"
    cp "$WF" "$SANDBOX/.github/workflows/build-and-test.yml"
    printf 'seed\n' > "$SANDBOX/README.md"
    git -C "$SANDBOX" add -A
    git -C "$SANDBOX" commit -q -m base
    git -C "$SANDBOX" checkout -q -b feature
}

teardown() {
    [ -n "${SANDBOX:-}" ] && rm -rf "$SANDBOX"
}

# commit_path <path> — create or append to <path> on the feature branch and commit.
commit_path() {
    mkdir -p "$SANDBOX/$(dirname "$1")"
    printf 'change\n' >> "$SANDBOX/$1"
    git -C "$SANDBOX" add -A
    git -C "$SANDBOX" commit -q -m "touch $1"
}

# detect — run the SUT over develop...HEAD in the sandbox; stdout only.
detect() {
    (cd "$SANDBOX" && bash "$SUT" develop...HEAD 2>/dev/null)
}

# job_line <job-id> <needle> — 1-based line of the first line containing <needle>
# inside job <job-id> of the sandbox workflow.
job_line() {
    awk -v job="  $1:" -v needle="$2" '
        $0 == job { in_job = 1; next }
        in_job && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { exit }
        in_job && index($0, needle) { print NR; exit }
    ' "$SANDBOX/.github/workflows/build-and-test.yml"
}

# rewrite_line <n> <sed-expr> — apply <sed-expr> to line <n> of the sandbox workflow, commit.
rewrite_line() {
    local f="$SANDBOX/.github/workflows/build-and-test.yml"
    sed -i.bak "${1}${2}" "$f" && rm -f "$f.bak"
    git -C "$SANDBOX" add -A
    git -C "$SANDBOX" commit -q -m "edit workflow line $1"
}

@test "every sanitizer-lane input path sets source_core_cpp=true" {
    local paths=(
        Source/Core/src/Tracker/Foo.cpp
        Source/Core/include/Foo.h
        Source/Plugins/Mcp/Bar.hpp
        cmake/Sanitizers.cmake
        .github/actions/setup-pinned-llvm/action.yml
        CMakePresets.json
        CMakeLists.txt
        Source/Core/CMakeLists.txt
        Source/Plugins/Mcp/CMakeLists.txt
        Source/Standalone/CMakeLists.txt
        .github/workflows/sanitizer-nightly.yml
        tests/Core/Foo.test.cpp
        tests/CMakeLists.txt
        tests/support/StubAiClient.h
        tests/test_main.cpp
    )
    local p failures=""
    for p in "${paths[@]}"; do
        git -C "$SANDBOX" reset -q --hard develop
        commit_path "$p"
        out="$(detect)"
        printf '%s\n' "$out" | grep -qx 'source_core_cpp=true' || failures="$failures $p"
    done
    [ -z "$failures" ] || {
        echo "source_core_cpp did not flip for:$failures" >&2
        return 1
    }
}

@test "a docs-only diff sets source_core_cpp=false and code=false" {
    commit_path "$FIXTURE_PLAN_DIR/some-plan.md"
    commit_path docs/self-improvement/categories/infra/2026-01-01-x.md
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=false'
    printf '%s\n' "$out" | grep -qx 'code=false'
}

@test "a Standalone-only C++ diff builds but skips the sanitizer lanes" {
    commit_path Source/Standalone/Main.cpp
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'code=true'
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=false'
}

@test "a diff only under tests/bats or tests/agent-eval skips the sanitizer lanes" {
    # Neither tree is compiled into the doctest rig.
    commit_path tests/bats/some_suite.bats
    commit_path tests/agent-eval/code-review/case.json
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=false'
}

@test "build-and-test.yml: a changed step inside a sanitizer job flips it" {
    # The ctest line of the UBSan lane does not itself say "sanitizer"; only the
    # job-range check can see it.
    n="$(job_line sanitizer-ubsan-pr 'ctest --output-on-failure')"
    [ -n "$n" ]
    run grep -qi sanitizer <(sed -n "${n}p" "$SANDBOX/.github/workflows/build-and-test.yml")
    [ "$status" -ne 0 ]
    rewrite_line "$n" 's/--output-on-failure/--output-on-failure --timeout 1800/'
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=true'
}

@test "build-and-test.yml: removing a step inside a sanitizer job flips it" {
    n="$(job_line sanitizer-asan 'timeout-minutes:')"
    [ -n "$n" ]
    rewrite_line "$n" 'd'
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=true'
}

@test "build-and-test.yml: a changed line mentioning sanitizer outside the lanes flips it" {
    n="$(job_line windows-msvc 'timeout-minutes:')"
    [ -n "$n" ]
    rewrite_line "$n" 's/$/ # raised for the sanitizer rig/'
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=true'
}

@test "build-and-test.yml: a sanitizer mention early in a large diff still flips it" {
    # Regression: a `... | grep -q` last stage SIGPIPEs its writer under pipefail,
    # so a large diff whose first changed line matched read as "no match".
    n="$(job_line windows-msvc 'timeout-minutes:')"
    [ -n "$n" ]
    local f="$SANDBOX/.github/workflows/build-and-test.yml"
    sed -i.bak "${n}s/\$/ # raised for the sanitizer rig/" "$f" && rm -f "$f.bak"
    seq 1 20000 | sed 's/^/# padding /' >> "$f"
    git -C "$SANDBOX" add -A
    git -C "$SANDBOX" commit -q -m "large edit"
    local i
    for i in 1 2 3 4 5; do
        detect | grep -qx 'source_core_cpp=true' || { echo "run $i read false" >&2; return 1; }
    done
}

@test "build-and-test.yml: an unrelated job edit does not run the sanitizer lanes" {
    n="$(job_line windows-msvc 'timeout-minutes:')"
    [ -n "$n" ]
    rewrite_line "$n" 's/timeout-minutes: [0-9]*/timeout-minutes: 47/'
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'code=true'
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=false'
}

@test "an unresolvable range fails safe: code=true source_core_cpp=true" {
    out="$(cd "$SANDBOX" && bash "$SUT" no-such-ref...HEAD 2>/dev/null)"
    printf '%s\n' "$out" | grep -qx 'code=true'
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=true'
    printf '%s\n' "$out" | grep -qx 'android_only=false'
}

@test "an android-only diff keeps android_only=true" {
    commit_path Source/Mobile/Android/Main.cpp
    commit_path "$FIXTURE_PLAN_DIR/mobile.md"
    out="$(detect)"
    printf '%s\n' "$out" | grep -qx 'android_only=true'
    printf '%s\n' "$out" | grep -qx 'source_core_cpp=false'
}

@test "stdout carries exactly the three outputs, in order" {
    commit_path Source/Core/src/Foo.cpp
    run detect
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 3 ]
    [[ "${lines[0]}" == code=* ]]
    [[ "${lines[1]}" == source_core_cpp=* ]]
    [[ "${lines[2]}" == android_only=* ]]
}

@test "usage error without a range exits 2" {
    run bash "$SUT"
    [ "$status" -eq 2 ]
}

@test "the changes job calls the script and keeps no inline copy of the classification" {
    grep -qF 'bash scripts/ci/detect-changes.sh "$range" >> "$GITHUB_OUTPUT"' "$WF"
    # The only inline source_core_cpp writes left are the two event-forcing ones.
    run grep -cF 'echo "source_core_cpp=' "$WF"
    [ "$output" -eq 2 ]
    ! grep -qE '^[[:space:]]+files=\$\(git diff --name-only' "$WF"
}

@test "nightly clang job builds and runs the instrumented rig under the pinned LLVM" {
    # The job body: from its id to the next top-level job.
    body="$(awk '/^  sanitizer-nightly:[[:space:]]*$/ { f = 1; print; next }
                 f && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { exit }
                 f { print }' "$NIGHTLY")"
    [ -n "$body" ]
    printf '%s\n' "$body" | grep -qF 'uses: ./.github/actions/setup-pinned-llvm'
    printf '%s\n' "$body" | grep -qE 'cmake --preset ninja-clang-asan .*-DSMATCHET_BUILD_TESTS=ON'
    printf '%s\n' "$body" | grep -qE 'cmake --preset ninja-clang-asan .*-DSMATCHET_SANITIZE_TESTS=ON'
    printf '%s\n' "$body" | grep -qE 'cmake --build --preset ninja-clang-asan --target .*SmatchetTests'
    printf '%s\n' "$body" | grep -qF './tests/SmatchetTests.exe'
    # A run that reports no test case must fail, not pass vacuously.
    printf '%s\n' "$body" | grep -qF 'test cases: +[1-9]'
}
