#!/usr/bin/env bats
# tests/bats/pre_push_format_delta.bats — the pre-push local CI delta-gate's
# clang-format step (step 3) must be DELTA-aware: pre-existing format drift in
# a touched file must not block a clean unrelated change, while a badly
# formatted NEW hunk still refuses (tooling
# 2026-07-05-pre-push-clang-format-whole-file-not-delta).
#
# Drives the real hook against a throwaway fixture repo whose base commit
# carries deliberate drift. Skips when clang-format / git-clang-format are
# absent (the hook fails open / falls back whole-file in that case).

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    HOOK="$REPO_ROOT/scripts/git-hooks/pre-push"
    FIX="$(mktemp -d)"
    cd "$FIX" || return 1
    git init -q -b develop
    git config user.email test@example.com
    git config user.name test
    printf 'BasedOnStyle: LLVM\n' > .clang-format
    mkdir -p Source/Core/src
    # Pre-existing drift the delta gate must grandfather.
    printf 'int  main( ){return   0;}\n' > Source/Core/src/foo.cpp
    git add -A && git commit -qm base
    git update-ref refs/remotes/origin/develop HEAD
    git checkout -qb feature
}

teardown() {
    cd / && rm -rf "$FIX"
}

require_tools() {
    command -v clang-format >/dev/null 2>&1 || skip "clang-format not on PATH"
    command -v git-clang-format >/dev/null 2>&1 || skip "git-clang-format not on PATH"
}

# Neutralise the sibling stages this suite is not testing (stage-neutraliser
# discipline, test/2026-06-24); empty stdin keeps stage A inert.
run_hook() {
    SMATCHET_ALLOW_UNLOCKED_PUSH=1 SMATCHET_ALLOW_MERGED_PR_PUSH=1 \
        SMATCHET_SKIP_REVIEW_MARKER=1 \
        bash "$HOOK" </dev/null
}

@test "clean new hunk in a file with pre-existing drift passes (delta, not whole-file)" {
    require_tools
    printf 'int  main( ){return   0;}\nint add(int a, int b) { return a + b; }\n' \
        > Source/Core/src/foo.cpp
    git add -A && git commit -qm clean-delta
    run run_hook
    [ "$status" -eq 0 ]
    [[ "$output" != *"clang-format reflow"* ]]
}

@test "badly formatted new hunk still refuses with the reflow line" {
    require_tools
    printf 'int  main( ){return   0;}\nint sub(int a,int b){return a-b ;}\n' \
        > Source/Core/src/foo.cpp
    git add -A && git commit -qm bad-delta
    run run_hook
    [ "$status" -eq 1 ]
    [[ "$output" == *"clang-format reflow"* ]]
}
