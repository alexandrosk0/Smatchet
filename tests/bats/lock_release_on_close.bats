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

    # Stub gh for the open-PR guard (`gh pr list --head <ref> --state open
    # --json … --jq …`): prints $STUB_OPEN_PRS verbatim — the --jq output shape,
    # one "<head owner>/<head repo> <number>" per line (unset → no open PR);
    # STUB_GH_FAIL=1 → the query fails. Every call is logged to $GH_LOG.
    export GH_LOG="$SANDBOX/gh.log"
    mkdir -p "$SANDBOX/bin"
    cat > "$SANDBOX/bin/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$GH_LOG"
if [ "$1" = "pr" ] && [ "$2" = "list" ]; then
    [ "${STUB_GH_FAIL:-0}" = "1" ] && { echo "HTTP 502" >&2; exit 1; }
    [ -n "${STUB_OPEN_PRS:-}" ] && printf '%s\n' "$STUB_OPEN_PRS"
    exit 0
fi
echo "stub gh: unhandled: $*" >&2
exit 99
STUB
    chmod +x "$SANDBOX/bin/gh"
    export PATH="$SANDBOX/bin:$PATH"
    unset STUB_OPEN_PRS STUB_GH_FAIL

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

# ---------- active-work guard: an OPEN PR on the same head keeps the locks ----------

@test "a close while the head branch still has another open PR releases nothing" {
    STUB_OPEN_PRS="o/r 2301" BASE_REPO="o/r" HEAD_REPO="o/r" release --branch feat/x
    [ "$status" -eq 0 ]
    held x-lock
    held y-lock
    [[ "$output" == *"still has open PR(s) #2301"* ]]
    grep -q -- '--repo o/r --head feat/x --state open' "$GH_LOG"
}

@test "an open PR from a FORK branch of the same name does not hold the lock" {
    STUB_OPEN_PRS="someone/r 2302" BASE_REPO="o/r" HEAD_REPO="o/r" release --branch feat/x
    [ "$status" -eq 0 ]
    gone x-lock
}

@test "a failed open-PR query releases nothing and exits 3" {
    STUB_GH_FAIL=1 release --branch feat/x
    [ "$status" -eq 3 ]
    held x-lock
    [[ "$output" == *"Could not list the open PRs"* ]]
}

@test "a release names the re-claim path for a later reopen" {
    release --branch feat/x
    [ "$status" -eq 0 ]
    gone x-lock
    [[ "$output" == *"If this PR is reopened"*"lock-claim.sh"* ]]
}

@test "--check-open prints active=true while an open PR remains and releases nothing" {
    STUB_OPEN_PRS="o/r 2303" HEAD_REPO="o/r" release --check-open feat/x
    [ "$status" -eq 0 ]
    [[ "$output" == *"active=true"* ]]
    held x-lock
    STUB_OPEN_PRS="" release --check-open feat/x
    [ "$status" -eq 0 ]
    [[ "$output" == *"active=false"* ]]
    STUB_GH_FAIL=1 release --check-open feat/x
    [ "$status" -eq 3 ]
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

@test "--slug against an unreachable remote exits 3 (not 'nothing to release')" {
    # A failing fetch is "absent" only when the remote answered with no such
    # ref; ls-remote failing (network/auth/bad URL) is an error.
    git -C "$CLONE" remote add broken "$SANDBOX/no-such-remote.git"
    LOCK_REMOTE=broken release --slug x-lock
    [ "$status" -eq 3 ]
    [[ "$output" == *"Could not reach broken"* ]]
    [[ "$output" != *"nothing to release"* ]]
    held x-lock
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

# ---------- lock-release-dispatch.yml wiring ----------

@test "dispatch workflow: workflow_dispatch with a required slug input" {
    grep -qE '^[[:space:]]*workflow_dispatch:' "$DISPATCH_WF"
    grep -qE '^[[:space:]]*slug:' "$DISPATCH_WF"
    grep -qE '^[[:space:]]*required:[[:space:]]*true' "$DISPATCH_WF"
}

@test "dispatch workflow: releases through the shared --slug path with contents: write" {
    grep -qE 'lock-release-on-close\.sh" --slug "\$SLUG"' "$DISPATCH_WF"
    grep -qE '^[[:space:]]*contents:[[:space:]]*write' "$DISPATCH_WF"
}

@test "dispatch workflow: the slug input reaches run: only through env (no template injection)" {
    grep -qE 'SLUG:[[:space:]]*\$\{\{[[:space:]]*inputs\.slug[[:space:]]*\}\}' "$DISPATCH_WF"
    # No `${{ … }}` expression may sit inside a run: script.
    run awk '
        /^[[:space:]]*run:/ { inrun = 1; indent = match($0, /[^ ]/); if ($0 ~ /\$\{\{/) print; next }
        inrun && NF && match($0, /[^ ]/) <= indent { inrun = 0 }
        inrun && /\$\{\{/ { print }
    ' "$DISPATCH_WF"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "dispatch workflow: every action is pinned to a full commit SHA" {
    run grep -cE '^[[:space:]]*(-[[:space:]]+)?uses:' "$DISPATCH_WF"
    [ "$output" -ge 1 ]
    run bash -c 'grep -E "^[[:space:]]*(-[[:space:]]+)?uses:" "$1" | grep -vE "@[0-9a-f]{40}([[:space:]]|\$)"' _ "$DISPATCH_WF"
    [ -z "$output" ]
}

@test "dispatch workflow: concurrency is per slug and never cancels (no dropped queued releases)" {
    # GitHub keeps ONE pending run per concurrency group; a fixed group made a
    # batch of dispatched releases drop all but the newest queued one.
    run grep -cE '^[[:space:]]*group:[[:space:]]*plan-lock-release-dispatch-\$\{\{[[:space:]]*inputs\.slug[[:space:]]*\}\}[[:space:]]*$' "$DISPATCH_WF"
    [ "$output" -eq 1 ]
    run grep -cE '^[[:space:]]*group:[[:space:]]*plan-lock-release-dispatch[[:space:]]*$' "$DISPATCH_WF"
    [ "$output" -eq 0 ]
    grep -qE '^[[:space:]]*cancel-in-progress:[[:space:]]*false' "$DISPATCH_WF"
}

@test "dispatch workflow: checks the release script out from develop, never an input ref" {
    grep -qE '^[[:space:]]*ref:[[:space:]]*develop[[:space:]]*$' "$DISPATCH_WF"
}
