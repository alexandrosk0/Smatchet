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
teardown() { rm -rf "$MAIN" "$NOGH" "${ORIGIN:-}" "${GHSTUB:-}" 2>/dev/null || true; }

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

# --- --remote: pushed-then-abandoned origin/* branches (tooling 2026-05-30) ---
# A bare repo plays origin. develop holds an OLD commit then a fresh one; one
# remote branch of each kind is pushed beside it, and a gh stub reports a PR for
# `with-pr` only. Exactly `abandoned` meets every condition.
remote_fixture() {
    ORIGIN="$(mktemp -d)/origin.git"
    git init -q --bare -b develop "$ORIGIN"
    git -C "$MAIN" remote add origin "$ORIGIN"
    local old; old="$(( $(date +%s) - 30 * 86400 )) +0000"
    ( cd "$MAIN" && echo old > o && git add o \
        && GIT_AUTHOR_DATE="$old" GIT_COMMITTER_DATE="$old" git commit -qm old-work )
    local old_sha; old_sha="$(git -C "$MAIN" rev-parse HEAD)"
    ( cd "$MAIN" && echo new > n && git add n && git commit -qm fresh-work )
    git -C "$MAIN" branch abandoned "$old_sha"        # merged + old + no PR -> flagged
    git -C "$MAIN" branch with-pr "$old_sha"          # has a PR
    git -C "$MAIN" branch asset-store "$old_sha"      # protected by config
    git -C "$MAIN" branch recent HEAD                 # merged, but last commit is today
    local ahead; ahead="$(GIT_AUTHOR_DATE="$old" GIT_COMMITTER_DATE="$old" \
        git -C "$MAIN" commit-tree "$old_sha^{tree}" -p "$old_sha" -m unmerged-work)"
    git -C "$MAIN" branch ahead "$ahead"              # old + no PR, but not on develop
    git -C "$MAIN" push -q origin develop abandoned with-pr asset-store recent ahead
    # Drop the local copies: the section is about ORIGIN refs only.
    git -C "$MAIN" branch -q -D abandoned with-pr asset-store recent ahead
    GHSTUB="$(mktemp -d)"
    printf '#!/usr/bin/env bash\nprintf "with-pr\\tOPEN\\n"\n' > "$GHSTUB/gh"
    chmod +x "$GHSTUB/gh"
    printf '{"vcs": {"protected_branches": ["asset-store"]}}\n' > "$GHSTUB/project.config.json"
}

audit_remote() { ( cd "$MAIN" && PATH="$GHSTUB:$PATH" PC_CONFIG_FILE="$GHSTUB/project.config.json" bash "$SCRIPT" "$@" ); }

@test "--remote flags only the old, merged, PR-less, unprotected origin branch" {
    remote_fixture
    run audit_remote --remote --age-days 7
    [ "$status" -eq 0 ]
    [[ "$output" == *"STALE REMOTE BRANCHES"* ]]
    [[ "$output" == *"origin/abandoned"*"30d"*"old-work"* ]]
    [[ "$output" != *"origin/with-pr"* ]]
    [[ "$output" != *"origin/asset-store"* ]]
    [[ "$output" != *"origin/recent"* ]]
    [[ "$output" != *"origin/ahead"* ]]
    [[ "$output" != *"origin/develop "* ]]
    [[ "$output" == *"1 stale remote branch(es)"* ]]
}

@test "--remote is report-only and --age-days moves the cut-off" {
    remote_fixture
    refs_before="$(git -C "$ORIGIN" for-each-ref)"
    run audit_remote --remote --age-days 45
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 stale remote branch(es)"* ]]
    [ "$(git -C "$ORIGIN" for-each-ref)" = "$refs_before" ]
}

@test "--age-days rejects a non-number" {
    run bash -c "cd '$MAIN' && bash '$SCRIPT' --no-fetch --remote --age-days old"
    [ "$status" -eq 2 ]
}

@test "without --remote the remote section is not printed" {
    remote_fixture
    run audit_remote
    [ "$status" -eq 0 ]
    [[ "$output" != *"STALE REMOTE BRANCHES"* ]]
}
