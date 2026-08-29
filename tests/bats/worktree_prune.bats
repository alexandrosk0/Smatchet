#!/usr/bin/env bats
# tests/bats/worktree_prune.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/worktree-prune.sh. The pure prune_decision guard is
# covered by --selftest; here we prove the real mutation on a temp repo + linked
# worktree, with `gh` stubbed to report a chosen branch MERGED.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/scripts/dev/worktree-prune.sh"
    MAIN="$(mktemp -d)/main"
    git init -q -b develop "$MAIN"
    git -C "$MAIN" config user.email t@local
    git -C "$MAIN" config user.name t
    ( cd "$MAIN" && echo seed > s && git add -A && git commit -qm seed )
    WT="$(mktemp -d)/wt-x"
    git -C "$MAIN" worktree add -q -b feat/x "$WT" >/dev/null 2>&1
    STUB="$(mktemp -d)"
    printf '#!/usr/bin/env bash\nprintf "feat/x\\tMERGED\\n"\n' > "$STUB/gh"
    chmod +x "$STUB/gh"
}
teardown() { rm -rf "$MAIN" "$WT" "$STUB" 2>/dev/null || true; }

# Run the script in $MAIN with gh stubbed (real PATH preserved for git/awk/etc).
prune() { ( cd "$MAIN" && PATH="$STUB:$PATH" bash "$SCRIPT" "$@" ); }

@test "--selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]; [[ "$output" == *PASS* ]]
}

# NB: assert via filesystem existence ([ -d ]), not by grepping `git worktree
# list` paths — on Windows git reports C:/... while $WT is the MSYS /tmp/... form,
# so a path grep is unreliable (and an absent-grep would trivially pass).

@test "dry-run (default) does NOT remove the merged worktree" {
    run prune
    [ "$status" -eq 0 ]
    [[ "$output" == *"would-reap"*"feat/x"* ]]
    [ -d "$WT" ]
}

@test "--apply removes a MERGED + clean worktree and its branch" {
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"reaped"*"feat/x"* ]]
    [ ! -d "$WT" ]
    run git -C "$MAIN" branch --list feat/x
    [ -z "$output" ]
}

@test "--apply SKIPS a dirty merged worktree (preserves uncommitted work)" {
    echo dirty > "$WT/uncommitted.txt"
    git -C "$WT" add -A
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"skip(dirty)"*"feat/x"* ]]
    [ -d "$WT" ]
}

@test "the develop integration tree is never reaped" {
    run prune --apply
    [ "$status" -eq 0 ]
    [ -d "$MAIN/.git" ]
}

# --- cmd_resync self-filter (finding #1958) ---------------------------------
# resync used to rewrite EVERY registry entry unconditionally, so running it in
# the shared integration tree silently re-baselined other LIVE sessions and blinded
# guard-head-drift.sh for them. These pin the three modes; the safety property is
# that a LIVE sibling is never clobbered without --all.

# worktree.sh derives REPO_ROOT from its OWN directory ($SCRIPT_DIR/../..), not
# from cwd — so invoking the repo copy from inside a temp tree would target the
# REAL repo and rewrite the developer's live session registry. Install a copy
# INSIDE the fixture so REPO_ROOT resolves to it. (Caught when the first cut of
# these tests did exactly that.)
resync_script() {  # <tree> — path to a worktree.sh whose REPO_ROOT is <tree>
    mkdir -p "$1/scripts/dev" "$1/agents/scripts/core"
    cp "$REPO_ROOT/scripts/dev/worktree.sh" "$1/scripts/dev/worktree.sh"
    cp "$REPO_ROOT/agents/scripts/core/session-registry-lib.sh" "$1/agents/scripts/core/" 2>/dev/null || true
    printf '%s/scripts/dev/worktree.sh' "$1"
}

resync_seed() {   # <tree>  — own entry + a live sibling + a dead-and-stale sibling
    local d="$1/.claude/.active-sessions" now; now="$(date +%s)"
    mkdir -p "$d"
    printf 'branch=old\nsha=dead\nppid=%s\nts=%s\n' "$$" "$now"           > "$d/mine"
    printf 'branch=old\nsha=dead\nppid=%s\nts=%s\n' "$$" "$now"           > "$d/live-sib"
    printf 'branch=old\nsha=dead\nppid=999999\nts=%s\n' "$((now - 99999))" > "$d/dead-sib"
}
resync_branch_of() { sed -n 's/^branch=//p' "$1/.claude/.active-sessions/$2" | head -1; }

@test "resync with a known session id rewrites only the caller's own entry" {
    local sc; sc="$(resync_script "$MAIN")"; resync_seed "$MAIN"
    ( cd "$MAIN" && CLAUDE_SESSION_ID=mine bash "$sc" resync ) >/dev/null 2>&1
    [ "$(resync_branch_of "$MAIN" mine)" = "develop" ]
    [ "$(resync_branch_of "$MAIN" live-sib)" = "old" ]
    [ "$(resync_branch_of "$MAIN" dead-sib)" = "old" ]
}

# selftest: asserts-failure — a LIVE sibling must survive a no-session-id resync;
# the pre-fix code rewrote it, blinding the drift guard for that session.
@test "resync without a session id never clobbers a live sibling" {
    local sc; sc="$(resync_script "$MAIN")"; resync_seed "$MAIN"
    run env -u CLAUDE_SESSION_ID -u SMATCHET_JANITOR_SELF_SESSION \
        bash -c "cd '$MAIN' && bash '$sc' resync"
    [ "$status" -eq 0 ]
    [ "$(resync_branch_of "$MAIN" live-sib)" = "old" ]
    [ "$(resync_branch_of "$MAIN" dead-sib)" = "develop" ]   # dead+stale is safe to take
    [[ "$output" == *"skipped (live sibling"* ]]
    [[ "$output" == *"--all"* ]]                             # names the escape hatch
}

@test "resync --all rewrites siblings and names each one" {
    local sc; sc="$(resync_script "$MAIN")"; resync_seed "$MAIN"
    run bash -c "cd '$MAIN' && CLAUDE_SESSION_ID=mine bash '$sc' resync --all"
    [ "$status" -eq 0 ]
    [ "$(resync_branch_of "$MAIN" mine)" = "develop" ]
    [ "$(resync_branch_of "$MAIN" live-sib)" = "develop" ]
    [ "$(resync_branch_of "$MAIN" dead-sib)" = "develop" ]
    [[ "$output" == *"overwriting sibling entry"* ]]
}
