#!/usr/bin/env bats
# tests/bats/guard_shared_tree.bats
# ----------------------------------------------------------------------------
# Bats tests for docs/harness/claude-code/hooks/guard-shared-tree.sh — the
# linked-worktree exemption (tooling self-improvement 2026-06-11). A live sibling
# is injected via a registry entry (ppid=1 + fresh ts → ts-freshness live arm).
#
# Asserts:
#   git -C <linked-worktree> merge   -> ALLOWED  (exempt: targets another tree)
#   git merge   (bare, shared tree)  -> BLOCKED  (would rug-pull the sibling)
#   git -C <integration-tree> merge  -> BLOCKED  (-C points back at shared HEAD)
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    HOOK="$REPO_ROOT/docs/harness/claude-code/hooks/guard-shared-tree.sh"

    # Fake integration tree (main worktree → .git is a DIRECTORY, so the guard arms).
    PROJ="$(mktemp -d)"
    git -C "$PROJ" init -q -b develop
    git -C "$PROJ" config user.email t@local
    git -C "$PROJ" config user.name t
    echo seed > "$PROJ/seed.txt"
    git -C "$PROJ" add -A
    git -C "$PROJ" commit -qm seed
    # A linked worktree off the integration tree.
    WT="$(mktemp -d)/wt"
    git -C "$PROJ" worktree add -q -b feature "$WT" >/dev/null 2>&1

    # A LIVE sibling session (ppid=1 → ts-freshness arm; fresh ts → live).
    REGDIR="$PROJ/.claude/.active-sessions"
    mkdir -p "$REGDIR"
    printf 'ppid=1\nts=%s\n' "$(date -u +%s)" > "$REGDIR/sibling-session"

    export CLAUDE_PROJECT_DIR="$PROJ"
}

teardown() {
    git -C "$PROJ" worktree remove --force "$WT" 2>/dev/null || true
    rm -rf "$PROJ" "$(dirname "$WT")" 2>/dev/null || true
}

# Run the hook with a command payload; echoes stdout, sets $status.
run_hook() {
    printf '{"tool_input":{"command":"%s"},"session_id":"mysid"}' "$1" | bash "$HOOK"
}

@test "git -C <linked-worktree> merge is EXEMPT (allowed) despite a live sibling" {
    run run_hook "git -C $WT merge origin/develop"
    [ "$status" -eq 0 ]
    [ -z "$output" ]   # no deny JSON
}

@test "bare git merge in the shared tree is BLOCKED (live sibling)" {
    run run_hook "git merge origin/develop"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}

@test "git -C <integration-tree> merge is BLOCKED (-C points back at shared HEAD)" {
    run run_hook "git -C $PROJ merge origin/develop"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}

@test "exemption fails CLOSED: -C to a non-worktree path does NOT exempt" {
    run run_hook "git -C /no/such/path/xyz merge origin/develop"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}
