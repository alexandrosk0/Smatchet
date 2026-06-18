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

# jq-built payload so commands containing quotes / $ / newlines stay valid JSON.
run_hook_jq() {
    jq -cn --arg c "$1" '{tool_input:{command:$c}, session_id:"mysid"}' | bash "$HOOK"
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

# ---- false-positive hardening (tooling.md 2026-06-18 :566 / :10) -------------

@test "FP: a git verb that only appears as an echo ARGUMENT is NOT denied" {
    run run_hook_jq "echo remember to git checkout main"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "FP: a git verb inside a quoted string is NOT denied" {
    run run_hook_jq 'echo "do git reset --hard now"'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "FP: a git verb in a heredoc BODY is NOT denied" {
    run run_hook_jq "$(printf 'cat <<EOF\ngit reset --hard\nEOF')"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "FP: git -C \$VAR (unexpanded) is treated as a worktree (allowed)" {
    run run_hook_jq 'git -C "$WT" merge origin/develop'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "cd <linked-worktree> && git merge is BLOCKED (cd is not exemptable; use git -C)" {
    # cd-based exemption was dropped as unsound (a later op can re-target the
    # shared tree). The canonical cross-worktree form is git -C <abs-path>.
    run run_hook_jq "cd $WT && git merge origin/develop"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}

@test "TP: a subshell-wrapped mutating op is still BLOCKED" {
    run run_hook_jq "(git reset --hard)"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}

# ---- ordering / command-position: a later -C, or a verb appearing only as an
# ---- argument, must NOT exempt an earlier real bare op (Cursor #1388) ---------

@test "ordering: a bare shared-tree op alongside a cd-to-worktree is still BLOCKED" {
    run run_hook_jq "git reset --hard && cd $WT && git merge origin/develop"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}

@test "cmd-position: a verb appearing only as an echo ARGUMENT does not force a deny" {
    # `... && echo git reset` must not be treated as a real op; the only real op
    # is the worktree-targeted merge, so the command stays EXEMPT.
    run run_hook_jq "git -C $WT merge origin/develop && echo git reset"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "ordering: a bare shared-tree op alongside a -C-worktree op is still BLOCKED" {
    run run_hook_jq "git -C $WT merge origin/develop && git reset --hard"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
}

@test "ordering: ALL ops -C-targeted at a worktree remains EXEMPT" {
    run run_hook_jq "git -C $WT merge origin/develop && git -C $WT reset --hard"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}
