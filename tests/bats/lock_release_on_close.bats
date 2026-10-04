#!/usr/bin/env bats
# tests/bats/lock_release_on_close.bats
# ----------------------------------------------------------------------------
# Regression suite for agents/scripts/core/lock-release-on-close.sh — the
# branch-keyed plan-lock release lock-cleanup.yml runs on every PR close, and
# the --slug release lock-release-dispatch.yml runs on demand.
#
# Runs the REAL script against a sandbox bare remote whose locks are claimed
# with the REAL lock-claim.sh (so the claim.json `.branch` field is exactly
# what production writes). Two feature locks (feat/x, feat/y) per test:
#   - a close of feat/x with no lock-slug line releases ONLY x's lock;
#   - a bare `holds-lock:` line in the body releases neither;
#   - develop/main and project.config.json vcs.protected_branches are never
#     released by branch match;
#   - a fork head (HEAD_REPO != BASE_REPO) releases nothing;
#   - --slug releases one named lock whatever branch claimed it.
# Also pins the lock-release-dispatch.yml wiring (input via env, --slug mode).
#
# Requires: bash, git, python(3), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPTS_DIR="$REPO_ROOT/agents/scripts/core"
    export SCRIPT="$SCRIPTS_DIR/lock-release-on-close.sh"
    export DISPATCH_WF="$REPO_ROOT/.github/workflows/lock-release-dispatch.yml"

    # Force the git-ref backend (lock-claim.sh dispatches to p4 otherwise).
    unset SMATCHET_LOCK_BACKEND SMATCHET_AGENT_VCS
    # The guards read these; a CI shell may already carry them.
    unset PR_BODY HEAD_REPO BASE_REPO PC_CONFIG_FILE SMATCHET_PROJECT_CONFIG

    SANDBOX="$(mktemp -d)"
    export SANDBOX
    export BARE="$SANDBOX/bare.git"
    export CLONE="$SANDBOX/clone"
    git init --quiet --bare "$BARE"
    git -C "$BARE" symbolic-ref HEAD refs/heads/develop
    git init --quiet "$SANDBOX/seed"
    git -C "$SANDBOX/seed" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m seed
    git -C "$SANDBOX/seed" push --quiet "$BARE" HEAD:refs/heads/develop
    git clone --quiet "$BARE" "$CLONE"
    git -C "$CLONE" config user.email t@t
    git -C "$CLONE" config user.name t

    export SMATCHET_LOCK_BYPASS_REPO_CHECK=1
    export AGENT_ID="bats-test"
    export WS_FILE="$SANDBOX/write-set.txt"
    printf 'src/a.cpp\n' > "$WS_FILE"

    claim x-lock feat/x
    claim y-lock feat/y
}

teardown() {
    rm -rf "${SANDBOX:-}"
}

# claim <slug> <branch> — claim refs/locks/<slug> on the sandbox remote as <branch>.
claim() {
    (cd "$CLONE" && LOCK_BRANCH="$2" bash "$SCRIPTS_DIR/lock-claim.sh" "$1" "$WS_FILE") >/dev/null 2>&1
}

# held <slug> — rc 0 when refs/locks/<slug> exists on the sandbox remote.
held() {
    [ -n "$(git -C "$BARE" for-each-ref "refs/locks/$1")" ]
}

# gone <slug> — rc 0 when refs/locks/<slug> is absent. A function, not a bare
# `! held`: bats ignores a `!`-negated status anywhere but the last line.
gone() {
    ! held "$1"
}

# release <args…> — run the script from the clone (env set by the caller).
release() {
    run bash -c 'cd "$1" && shift && bash "$@"' _ "$CLONE" "$SCRIPT" "$@"
}

@test "precondition: both sandbox locks are held" {
    held x-lock
    held y-lock
}

@test "a no-slug close of feat/x releases only feat/x's lock" {
    release --branch feat/x
    [ "$status" -eq 0 ]
    gone x-lock
    held y-lock
    [[ "$output" == *"released=1, failed=0"* ]]
}

@test "the claim.json is logged before the lock is deleted" {
    release --branch feat/x
    [ "$status" -eq 0 ]
    [[ "$output" == *"Releasing refs/locks/x-lock"* ]]
    [[ "$output" == *'"branch":"feat/x"'* ]]
}

@test "--branch=<ref> form behaves like --branch <ref>" {
    release --branch=feat/y
    [ "$status" -eq 0 ]
    held x-lock
    gone y-lock
}

@test "a bare holds-lock: line releases neither lock" {
    PR_BODY="$(printf 'Intermediate slice.\n\nholds-lock: x-lock\n')" release --branch feat/x
    [ "$status" -eq 0 ]
    held x-lock
    held y-lock
    [[ "$output" == *"holds-lock"* ]]
}

@test "the template's commented holds-lock placeholder does not block release" {
    PR_BODY="$(printf '%s\n' '<!-- holds-lock: your-slug-here (use this on stacked-intermediate PRs) -->')" \
        release --branch feat/x
    [ "$status" -eq 0 ]
    gone x-lock
}

@test "a lock claimed under develop is never released by branch match" {
    claim shim-lock develop
    release --branch develop
    [ "$status" -eq 0 ]
    held shim-lock
    [[ "$output" == *"integration/protected branch"* ]]
}

@test "a lock claimed under main is never released by branch match" {
    claim main-lock main
    release --branch main
    [ "$status" -eq 0 ]
    held main-lock
}

@test "a project.config.json vcs.protected_branches entry is never released" {
    claim train-lock release-train
    printf '{"vcs":{"protected_branches":["release-train"]}}\n' > "$SANDBOX/project.config.json"
    PC_CONFIG_FILE="$SANDBOX/project.config.json" release --branch release-train
    [ "$status" -eq 0 ]
    held train-lock
}

@test "a fork head (HEAD_REPO != BASE_REPO) releases nothing" {
    BASE_REPO="o/r" HEAD_REPO="someone/r" release --branch feat/x
    [ "$status" -eq 0 ]
    held x-lock
    [[ "$output" == *"fork branch"* ]]
}

@test "a deleted fork (empty HEAD_REPO) releases nothing" {
    BASE_REPO="o/r" HEAD_REPO="" release --branch feat/x
    [ "$status" -eq 0 ]
    held x-lock
}

@test "a same-repo head (HEAD_REPO == BASE_REPO) releases its lock" {
    BASE_REPO="o/r" HEAD_REPO="o/r" release --branch feat/x
    [ "$status" -eq 0 ]
    gone x-lock
    held y-lock
}

@test "a branch holding several locks releases all of them" {
    claim x-second feat/x
    release --branch feat/x
    [ "$status" -eq 0 ]
    gone x-lock
    gone x-second
    held y-lock
}

@test "a branch holding no lock is a clean no-op" {
    release --branch feat/none
    [ "$status" -eq 0 ]
    held x-lock
    held y-lock
    [[ "$output" == *"released=0, failed=0"* ]]
}

@test "--slug releases one named lock whatever branch claimed it" {
    claim shim-lock develop
    release --slug shim-lock
    [ "$status" -eq 0 ]
    gone shim-lock
    held x-lock
    [[ "$output" == *'"branch":"develop"'* ]]
}

@test "--slug on an absent lock is a clean no-op" {
    release --slug no-such-lock
    [ "$status" -eq 0 ]
    [[ "$output" == *"nothing to release"* ]]
}

@test "--slug rejects a slug outside the lock grammar" {
    release --slug Bad_Slug
    [ "$status" -eq 2 ]
    [[ "$output" == *"invalid slug"* ]]
    held x-lock
}

@test "no mode argument is a usage error" {
    release
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}

@test "a remote that is not this project's repo is refused without the bypass" {
    SMATCHET_LOCK_BYPASS_REPO_CHECK=0 release --branch feat/x
    [ "$status" -eq 2 ]
    [[ "$output" == *"does not look like"* ]]
    held x-lock
}
