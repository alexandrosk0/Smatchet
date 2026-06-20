#!/usr/bin/env bats
# tests/bats/guard_plan_lock.bats
# ----------------------------------------------------------------------------
# Bats tests for docs/harness/claude-code/hooks/guard-plan-lock.sh — Layer A of
# the force-on-contention plan-lock enforcement.
#
# Siblings are injected as registry entries (ppid=1 + fresh ts -> ts-freshness
# live arm). The lock table is injected via LTC_ROWS_OVERRIDE (pre-flattened TSV
# rows: branch<TAB>epoch<TAB>slug<TAB>path) so no network / locks-show is needed.
# LTC_COUNT_TTL=0 makes every sibling-count recompute (no cross-test cache).
#
# Asserts: solo never blocks; >=1 sibling + uncovered -> deny; covered by a
# MY-BRANCH lock (exact or dir-prefix) -> allow; an OTHER-branch lock is not my
# cover (the branch-key / owner-collision canary); union-of-distinct sibling
# count dedups a sid across regdirs; bypass + detached-HEAD + out-of-repo all
# fail open.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    HOOK="$REPO_ROOT/docs/harness/claude-code/hooks/guard-plan-lock.sh"

    # Canonical path (pwd -P) so `git rev-parse --show-toplevel` matches the
    # file_path prefix even where TMPDIR is a symlink (macOS /tmp -> /private).
    PROJ="$(mktemp -d)"; PROJ="$(cd "$PROJ" && pwd -P)"
    git -C "$PROJ" init -q -b develop
    git -C "$PROJ" config user.email t@local
    git -C "$PROJ" config user.name t
    echo seed > "$PROJ/seed.txt"; git -C "$PROJ" add -A; git -C "$PROJ" commit -qm seed
    # A linked worktree, for the cross-worktree sibling-count tests.
    WT="$(mktemp -d)/wt"
    git -C "$PROJ" worktree add -q -b feat/wt "$WT" >/dev/null 2>&1
    WT="$(cd "$WT" && pwd -P)"

    REGDIR="$PROJ/.claude/.active-sessions";     mkdir -p "$REGDIR"
    WT_REGDIR="$WT/.claude/.active-sessions";     mkdir -p "$WT_REGDIR"

    # Synthetic lock table: a develop (MINE) lock + a feat/x (OTHER) lock.
    ROWS="$(mktemp)"
    {
        printf 'develop\t1781049600\tmine\tSource/Core/src/Sync/Foo.cpp\n'
        printf 'develop\t1781049600\tmine\tSource/Core/src/Persistence/\n'
        printf 'feat/x\t1781049600\tother\tSource/Core/src/Tracker/Bar.cpp\n'
    } > "$ROWS"

    export CLAUDE_PROJECT_DIR="$PROJ"
    export LTC_ROWS_OVERRIDE="$ROWS"
    export LTC_COUNT_TTL=0
    export SMATCHET_REGISTRY_OS=posix
    export SMATCHET_REGISTRY_FRESH_SECS=1800
    unset SMATCHET_REQUIRE_PLAN_LOCK
}

teardown() {
    git -C "$PROJ" worktree remove --force "$WT" 2>/dev/null || true
    rm -rf "$PROJ" "$(dirname "$WT")" "$ROWS" 2>/dev/null || true
}

# A live registry entry (ppid=1 -> ts arm; fresh ts -> live).
reg() { printf 'ppid=1\nts=%s\n' "$(date -u +%s)" > "$1/$2"; }

# Run the hook for an edit to <repo-relative-path> (made absolute under PROJ).
run_hook() { # $1 = repo-relative path
    jq -cn --arg fp "$PROJ/$1" \
        '{tool_name:"Edit", tool_input:{file_path:$fp}, session_id:"mysid"}' \
        | bash "$HOOK"
}
# Run the hook with an explicit absolute file_path (for out-of-repo tests).
run_hook_abs() { # $1 = absolute path
    jq -cn --arg fp "$1" \
        '{tool_name:"Edit", tool_input:{file_path:$fp}, session_id:"mysid"}' \
        | bash "$HOOK"
}

@test "solo (0 siblings) never blocks, even an uncovered path" {
    run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "solo: own sid double-registered across regdirs still dedups to 0 (allow)" {
    reg "$REGDIR" mysid
    reg "$WT_REGDIR" mysid
    run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "contended + uncovered path is DENIED (threshold is >=1 sibling)" {
    reg "$REGDIR" sibling-a
    run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
    [[ "$output" == *"1 other live session"* ]]
}

@test "contended + path covered by MY-branch lock is ALLOWED" {
    reg "$REGDIR" sibling-a
    run run_hook "Source/Core/src/Sync/Foo.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "contended + path under a MY-branch dir-prefix lock is ALLOWED" {
    reg "$REGDIR" sibling-a
    run run_hook "Source/Core/src/Persistence/Db.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "branch-key canary: an OTHER-branch lock is NOT my cover -> DENY" {
    reg "$REGDIR" sibling-a
    run run_hook "Source/Core/src/Tracker/Bar.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}

@test "cross-worktree: 2 distinct siblings across two regdirs are counted" {
    reg "$REGDIR" sibling-a
    reg "$WT_REGDIR" sibling-b
    run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
    [[ "$output" == *"2 other live session"* ]]
}

@test "dedup: the SAME sibling sid in two regdirs counts as 1, not 2" {
    reg "$REGDIR" dupe-sib
    reg "$WT_REGDIR" dupe-sib
    run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
    [[ "$output" == *"1 other live session"* ]]   # union-of-distinct, not a sum
}

@test "bypass: SMATCHET_REQUIRE_PLAN_LOCK=0 allows a contended uncovered edit" {
    reg "$REGDIR" sibling-a
    SMATCHET_REQUIRE_PLAN_LOCK=0 run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "fail-open: detached HEAD allows even when contended + uncovered" {
    reg "$REGDIR" sibling-a
    git -C "$PROJ" checkout -q --detach
    run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "fail-open: an edit OUTSIDE the repo is not gated" {
    reg "$REGDIR" sibling-a
    run run_hook_abs "$(dirname "$PROJ")/outside-file.txt"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "no file_path -> allow (nothing to gate)" {
    reg "$REGDIR" sibling-a
    run bash -c "printf '%s' '{\"tool_name\":\"Edit\",\"tool_input\":{},\"session_id\":\"mysid\"}' | bash '$HOOK'"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "deny output is a single valid PreToolUse deny JSON object" {
    reg "$REGDIR" sibling-a
    run run_hook "Source/Core/src/Sync/Unlisted.cpp"
    [ "$status" -eq 0 ]
    # Valid JSON with the deny decision and the remediation one-liner.
    echo "$output" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
    [[ "$output" == *"lock-claim.sh"* ]]
    [[ "$output" == *"SMATCHET_REQUIRE_PLAN_LOCK=0"* ]]
}
