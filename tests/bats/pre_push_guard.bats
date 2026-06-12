#!/usr/bin/env bats
# tests/bats/pre_push_guard.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/git-hooks/pre-push (A) — the direct-push-to-protected-
# branch hard-stop. Pre-push ref updates are fed on stdin
# ("<local_ref> <local_sha> <remote_ref> <remote_sha>"); `gh` is stubbed to a
# no-op so the (B) merged-PR check makes no network call and cleanly exits 0.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    HOOK="$REPO_ROOT/scripts/git-hooks/pre-push"
    TMP="$(mktemp -d)"
    git -C "$TMP" init -q -b feature
    git -C "$TMP" config user.email t@local
    git -C "$TMP" config user.name t
    ( cd "$TMP" && echo x > f && git add -A && git commit -qm x )
    STUB="$TMP/stub"; mkdir -p "$STUB"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$STUB/gh"; chmod +x "$STUB/gh"
    SHA="1111111111111111111111111111111111111111"
    ZERO="0000000000000000000000000000000000000000"
}

teardown() { rm -rf "$TMP"; }

# run_push <refline> [env-assignment]
run_push() {
    printf '%s\n' "$1" | ( cd "$TMP" && env ${2:-} PATH="$STUB:$PATH" bash "$HOOK" origin url )
}

@test "push to develop is REFUSED" {
    run run_push "refs/heads/feature $SHA refs/heads/develop $SHA"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSING direct push to 'develop'"* ]]
}

@test "push to main is REFUSED" {
    run run_push "refs/heads/feature $SHA refs/heads/main $SHA"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSING direct push to 'main'"* ]]
}

@test "HEAD:develop (renamed destination) is REFUSED" {
    run run_push "HEAD $SHA refs/heads/develop $SHA"
    [ "$status" -eq 1 ]
}

@test "SMATCHET_ALLOW_DEVELOP_PUSH=1 overrides the refusal" {
    run run_push "refs/heads/feature $SHA refs/heads/develop $SHA" "SMATCHET_ALLOW_DEVELOP_PUSH=1"
    [ "$status" -eq 0 ]
}

@test "push to a feature branch is allowed" {
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA"
    [ "$status" -eq 0 ]
}

@test "a develop branch DELETE is ignored (not a content push)" {
    run run_push "(delete) $ZERO refs/heads/develop $SHA"
    [ "$status" -eq 0 ]
}
