#!/usr/bin/env bats
# tests/bats/guard_head_drift.bats
# ----------------------------------------------------------------------------
# Bats tests for docs/harness/claude-code/hooks/guard-head-drift.sh — the
# PreToolUse HEAD-drift + no-direct-commit guard.
#
# Covers the tooling.md P2 fix (PRs #916/#936 friction):
#   - `git -C <linked-worktree-on-feature-branch> commit` from an
#     integration-tree-rooted session is ALLOWED (the commit never touches the
#     integration tree).
#   - A -C-less commit on develop in the integration tree stays DENIED, for
#     BOTH the Bash and the PowerShell tool (the PowerShell bypass gap).
#   - Conservative residue: -C at the integration tree itself, -C at a
#     worktree sitting on a protected branch, and mixed safe+unsafe compound
#     commands all stay DENIED.
#   - Drift deny: still fires for in-tree ops after HEAD moves, but a
#     -C-worktree-targeted op is exempt (cannot move this tree's HEAD).
#
# Requires: bash, git, jq, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export HOOK="$REPO_ROOT/docs/harness/claude-code/hooks/guard-head-drift.sh"

    # Sandbox: a "main" repo (integration tree, on develop) + a linked worktree
    # on a feature branch — per-test isolation.
    SANDBOX="$(mktemp -d)"
    export SANDBOX
    export MAIN="$SANDBOX/main"
    export WT="$SANDBOX/wt"

    git init --quiet -b develop "$MAIN"
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m seed
    git -C "$MAIN" worktree add --quiet -b feat/x "$WT" >/dev/null 2>&1

    # Session baseline entry (what SessionStart records).
    export SID="bats-test-session"
    mkdir -p "$MAIN/.claude/.active-sessions"
    {
        echo "branch=$(git -C "$MAIN" symbolic-ref --short HEAD)"
        echo "sha=$(git -C "$MAIN" rev-parse HEAD)"
    } > "$MAIN/.claude/.active-sessions/$SID"

    export CLAUDE_PROJECT_DIR="$MAIN"
    unset SMATCHET_ACK_BRANCH_DRIFT || true
}

teardown() {
    git -C "$MAIN" worktree remove --force "$WT" >/dev/null 2>&1 || true
    rm -rf "$SANDBOX"
}

# Feed the hook a PreToolUse JSON event; output captured by `run`.
invoke_hook() { # $1 = tool_name, $2 = command
    jq -cn --arg sid "$SID" --arg tool "$1" --arg cmd "$2" \
        '{session_id:$sid, tool_name:$tool, tool_input:{command:$cmd}}' \
        | bash "$HOOK"
}

assert_denied() {
    [ "$status" -eq 0 ]
    echo "$output" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
}

assert_allowed() {
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "Bash: plain commit on develop in integration tree is denied" {
    run invoke_hook Bash "git commit -m x"
    assert_denied
}

@test "PowerShell: plain commit on develop in integration tree is denied (bypass gap closed)" {
    run invoke_hook PowerShell "git commit -m x"
    assert_denied
}

@test "Bash: git -C <linked worktree on feature branch> commit is allowed" {
    run invoke_hook Bash "git -C $WT commit -m x"
    assert_allowed
}

@test "PowerShell: git -C <linked worktree on feature branch> commit is allowed" {
    run invoke_hook PowerShell "git -C $WT commit -m x"
    assert_allowed
}

@test "Bash: git -C with double-quoted worktree path is allowed" {
    run invoke_hook Bash "git -C \"$WT\" commit -m x"
    assert_allowed
}

@test "Bash: git -C at the integration tree itself stays denied" {
    run invoke_hook Bash "git -C $MAIN commit -m x"
    assert_denied
}

@test "Bash: git -C at a worktree on a protected branch stays denied" {
    git -C "$WT" checkout --quiet -B main
    run invoke_hook Bash "git -C $WT commit -m x"
    assert_denied
}

@test "Bash: compound command mixing safe -C commit and bare commit stays denied" {
    run invoke_hook Bash "git -C $WT commit -m x && git commit -m y"
    assert_denied
}

@test "Bash: non-commit command passes untouched" {
    run invoke_hook Bash "git -C $WT status"
    assert_allowed
}

# ---- separate-repo (-C <unrelated tmprepo>) exemption -----------------------
# A `git -C <path>` whose target is a wholly SEPARATE repository (its
# git-common-dir differs from the integration tree's) cannot move this session's
# HEAD, so it must be ALLOWED even when that throwaway repo sits on develop/main.
# Previously DENIED because a freshly `git init`-ed repo has a `.git` DIRECTORY
# (not a worktree file pointer), tripping the linked-worktree-only test and
# false-blocking bats fixtures that drive an unrelated mktemp repo.

@test "Bash: git -C <unrelated tmprepo on develop> commit is allowed (separate repo)" {
    THROWAWAY="$BATS_TMPDIR/throwaway-$$"
    rm -rf "$THROWAWAY"
    git init --quiet -b develop "$THROWAWAY"
    run invoke_hook Bash "git -C $THROWAWAY commit -m x"
    rm -rf "$THROWAWAY"
    assert_allowed
}

@test "Bash: bare commit on develop still DENIED while an unrelated tmprepo commit is allowed" {
    # The same protected-tree bare commit that the false-positive report is about
    # must remain GUARDED — only the unrelated -C target is newly allowed.
    THROWAWAY="$BATS_TMPDIR/throwaway2-$$"
    rm -rf "$THROWAWAY"
    git init --quiet -b main "$THROWAWAY"
    run invoke_hook Bash "git -C $THROWAWAY commit -m x"
    assert_allowed
    run invoke_hook Bash "git commit -m y"
    rm -rf "$THROWAWAY"
    assert_denied
}

@test "Bash: drifted HEAD denies an in-tree HEAD-moving op" {
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m drift
    run invoke_hook Bash "git pull"
    assert_denied
}

@test "Bash: drifted HEAD denies an op terminated by a separator, not a space (CR-947-1)" {
    # `git pull;x`, `git pull&`, `git pull|cat` — the op token's trailing
    # boundary must accept the same separator class as the leading one, or
    # the drift guard is defeated by simply omitting the space.
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m drift
    run invoke_hook Bash "git pull;true"
    assert_denied
    run invoke_hook Bash "git pull&"
    assert_denied
    run invoke_hook Bash "git pull|cat"
    assert_denied
}

@test "Bash: no-drift direct-commit guard also catches separator-terminated commit (CR-947-1)" {
    run invoke_hook Bash "git commit;true"
    assert_denied
}

@test "PowerShell: git.exe commit is denied (CR-953 review - previously failed open)" {
    run invoke_hook PowerShell "git.exe commit -m x"
    assert_denied
}

@test "PowerShell: quoted full-path git.exe via call operator is denied (CR-953 review)" {
    run invoke_hook PowerShell "& \"C:\\Program Files\\Git\\bin\\git.exe\" commit -m x"
    assert_denied
}

@test "Bash: subshell-wrapped commit is denied (CR-953 review - previously failed open)" {
    run invoke_hook Bash "(git commit -m x)"
    assert_denied
    run invoke_hook Bash "echo \$(git commit -m x)"
    assert_denied
}

@test "Bash: git.exe -C <worktree> commit is still allowed (exemption survives the .exe form)" {
    run invoke_hook Bash "git.exe -C $WT commit -m x"
    assert_allowed
}

@test "Bash: drifted HEAD still allows a -C-worktree-targeted commit" {
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m drift
    run invoke_hook Bash "git -C $WT commit -m x"
    assert_allowed
}

@test "Edit: drifted HEAD denies a write" {
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m drift
    run bash -c "jq -cn --arg sid \"$SID\" '{session_id:\$sid, tool_name:\"Edit\", tool_input:{file_path:\"x\"}}' | bash \"$HOOK\""
    [ "$status" -eq 0 ]
    echo "$output" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
}

# ---- quoted -C path WITH SPACES (tooling.md :328 / :175 fail-OPEN) ----------

@test "Bash: git -C \"<spaced worktree>\" commit on a feature branch is allowed" {
    WTSP="$SANDBOX/wt space"
    git -C "$MAIN" worktree add --quiet -b feat/sp "$WTSP" >/dev/null 2>&1
    run invoke_hook Bash "git -C \"$WTSP\" commit -m x"
    assert_allowed
}

@test "Bash: git -C \"<spaced worktree on protected branch>\" commit stays DENIED (no fail-open)" {
    # If the spaced quoted path failed to parse, the commit would slip past
    # un-matched (fail-OPEN). It must be recognised AND blocked on a protected
    # branch — proving the quoted-path grammar matches.
    WTSP="$SANDBOX/wt space"
    git -C "$MAIN" worktree add --quiet -b main2 "$WTSP" >/dev/null 2>&1
    git -C "$WTSP" checkout --quiet -B main
    run invoke_hook Bash "git -C \"$WTSP\" commit -m x"
    assert_denied
}

# ---- Edit/Write worktree exemption under drift (tooling.md :70) --------------

@test "Edit: drifted HEAD ALLOWS a write to a linked-worktree file" {
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m drift
    run bash -c "jq -cn --arg sid \"$SID\" --arg f \"$WT/foo.txt\" '{session_id:\$sid, tool_name:\"Edit\", tool_input:{file_path:\$f}}' | bash \"$HOOK\""
    assert_allowed
}

@test "Edit: drifted HEAD still DENIES a write to an integration-tree file" {
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m drift
    run bash -c "jq -cn --arg sid \"$SID\" --arg f \"$MAIN/foo.txt\" '{session_id:\$sid, tool_name:\"Edit\", tool_input:{file_path:\$f}}' | bash \"$HOOK\""
    assert_denied
}

@test "no session baseline entry -> allow (never false-block)" {
    rm -f "$MAIN/.claude/.active-sessions/$SID"
    run invoke_hook Bash "git commit -m x"
    assert_allowed
}

@test "SMATCHET_ACK_BRANCH_DRIFT=1 overrides the deny" {
    SMATCHET_ACK_BRANCH_DRIFT=1 run invoke_hook Bash "git commit -m x"
    assert_allowed
}
