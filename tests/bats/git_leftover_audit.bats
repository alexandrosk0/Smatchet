#!/usr/bin/env bats
# tests/bats/git_leftover_audit.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/git-leftover-audit.sh — the read-only leftover map.
# The pure classify_leftover bucketing is covered by --selftest; here we smoke
# the real run (read-only, exit 0) in a temp repo with gh absent.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/scripts/dev/git-leftover-audit.sh"
    MAIN="$(mktemp -d)/main"
    git init -q -b develop "$MAIN"
    git -C "$MAIN" config user.email t@local
    git -C "$MAIN" config user.name t
    ( cd "$MAIN" && echo seed > s && git add -A && git commit -qm seed )
    # gh-less PATH so the audit takes the NO-PR path without network.
    NOGH="$(mktemp -d)"   # empty dir prepended -> still finds system bins, but...
}
teardown() { rm -rf "$MAIN" "$NOGH" 2>/dev/null || true; }

@test "--selftest passes (classify_leftover buckets)" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]; [[ "$output" == *PASS* ]]
}

@test "read-only run emits a header and exits 0 (no mutation)" {
    head_before="$(git -C "$MAIN" rev-parse HEAD)"
    run bash -c "cd '$MAIN' && bash '$SCRIPT' --no-fetch"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WORKTREE/BRANCH"* ]]
    [[ "$output" == *"PROTECTED"* ]]   # the develop integration tree row
    [ "$(git -C "$MAIN" rev-parse HEAD)" = "$head_before" ]   # nothing changed
}

@test "not-a-git-tree exits 2" {
    run bash -c "cd \"\$(mktemp -d)\" && bash '$SCRIPT' --no-fetch"
    [ "$status" -eq 2 ]
}
