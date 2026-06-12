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
