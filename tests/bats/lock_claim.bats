#!/usr/bin/env bats
# tests/bats/lock_claim.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/lock-claim.sh — argument validation + happy path
# + lock-held rejection. Complementary to agents/scripts/core/test-lock-primitives.sh
# (the existing end-to-end multi-clone integration test); this file isolates
# each case in its own bats setup so a single failing argument-validation case
# doesn't poison the whole run.
#
# Per docs/evaluation/agentic-infrastructure-2026-05-23.md punch-list item 6.
#
# Requires: bash, git, jq, python3 (or python), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPTS_DIR="$REPO_ROOT/agents/scripts/core"

    # Force git-ref backend regardless of session env — the operator may run
    # bats with SMATCHET_LOCK_BACKEND=p4-counter set, which dispatches to
    # lock-claim-p4.sh and changes the output shape this file asserts on.
    unset SMATCHET_LOCK_BACKEND SMATCHET_AGENT_VCS

    # Sandbox bare repo + worker clone for each test case. Per-test isolation
    # prevents cross-test ref pollution.
    SANDBOX="$(mktemp -d)"
    export SANDBOX
    export BARE="$SANDBOX/bare.git"
    export CLONE="$SANDBOX/clone"

    git init --quiet --bare "$BARE"
    # Set bare HEAD so clones don't warn about "remote HEAD refers to
    # nonexistent ref" — cosmetic but keeps bats output clean.
    git -C "$BARE" symbolic-ref HEAD refs/heads/develop
    # Seed the bare repo with an initial commit so clones don't end up empty.
    SEED="$SANDBOX/seed"
    git init --quiet "$SEED"
    git -C "$SEED" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m seed
    git -C "$SEED" push --quiet "$BARE" HEAD:refs/heads/develop

    git clone --quiet "$BARE" "$CLONE"
    git -C "$CLONE" config user.email "t@t"
    git -C "$CLONE" config user.name "t"

    # Bypass the Smatchet remote-URL check — sandbox uses a local file URL.
    export SMATCHET_LOCK_BYPASS_REPO_CHECK=1
    export AGENT_ID="bats-test"
    export LOCK_BRANCH="develop"
    # Default write-set file — most tests need one.
    export WS_FILE="$SANDBOX/write-set.txt"
    printf 'Source/Core/include/Foo.h\nSource/Core/src/Foo.cpp\n' > "$WS_FILE"
}

teardown() {
    rm -rf "$SANDBOX"
}

# ---------- Argument validation ----------

@test "lock-claim: missing args returns 2" {
    run bash "$SCRIPTS_DIR/lock-claim.sh"
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}

@test "lock-claim: only one arg returns 2" {
    run bash "$SCRIPTS_DIR/lock-claim.sh" only-one
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}

@test "lock-claim: invalid slug uppercase returns 2" {
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' BadSlug '$WS_FILE'"
    [ "$status" -eq 2 ]
    [[ "$output" == *"invalid slug"* ]]
}

@test "lock-claim: invalid slug with underscore returns 2" {
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' bad_slug '$WS_FILE'"
    [ "$status" -eq 2 ]
    [[ "$output" == *"invalid slug"* ]]
}

@test "lock-claim: invalid slug starting with hyphen returns 2" {
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' -leading-hyphen '$WS_FILE'"
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]] || [[ "$output" == *"invalid slug"* ]]
}

@test "lock-claim: slug exceeding 64 chars returns 2" {
    local long_slug
    long_slug=$(printf 'a%.0s' {1..65})
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' '$long_slug' '$WS_FILE'"
    [ "$status" -eq 2 ]
    [[ "$output" == *"invalid slug"* ]]
}

@test "lock-claim: missing write-set file returns 2" {
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' valid-slug /nonexistent/path"
    [ "$status" -eq 2 ]
    [[ "$output" == *"write-set file not found"* ]]
}

@test "lock-claim: not inside a git checkout returns 2" {
    run bash -c "cd '$SANDBOX' && bash '$SCRIPTS_DIR/lock-claim.sh' valid-slug '$WS_FILE'"
    [ "$status" -eq 2 ]
    # $SANDBOX itself is not a git checkout; either the rev-parse fails or
    # the remote.origin.url probe fails — both are exit 2 cases.
    [[ "$output" == *"not inside a git"* ]] || [[ "$output" == *"remote 'origin' not configured"* ]]
}

# ---------- Remote-URL safety check ----------

@test "lock-claim: non-Smatchet remote without BYPASS returns 2" {
    unset SMATCHET_LOCK_BYPASS_REPO_CHECK
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' valid-slug '$WS_FILE'"
    [ "$status" -eq 2 ]
    [[ "$output" == *"does not look like a Smatchet repo"* ]]
}

@test "lock-claim: BYPASS=1 lets non-Smatchet remote through" {
    export SMATCHET_LOCK_BYPASS_REPO_CHECK=1
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' valid-slug '$WS_FILE'"
    [ "$status" -eq 0 ]
    [[ "$output" == *"claimed at"* ]]
}

# ---------- Happy path + CAS ----------

@test "lock-claim: fresh slug succeeds (exit 0 + claim message)" {
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' fresh-slug '$WS_FILE'"
    [ "$status" -eq 0 ]
    [[ "$output" == *"refs/locks/fresh-slug claimed at"* ]]
    [[ "$output" == *"owner=bats-test"* ]]
    [[ "$output" == *"branch=develop"* ]]
}

@test "lock-claim: integration-branch owner emits wrong-tree warning" {
    # Claiming with the owner branch resolved to develop/main is almost always a
    # wrong-tree claim (the plan-lock self-collision class) — a loud warning fires.
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' warn-slug '$WS_FILE'"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARNING: lock owner branch resolves to 'develop'"* ]]
}

@test "lock-claim: feature-branch owner emits no wrong-tree warning" {
    run bash -c "cd '$CLONE' && LOCK_BRANCH=feat/some-slice bash '$SCRIPTS_DIR/lock-claim.sh' quiet-slug '$WS_FILE'"
    [ "$status" -eq 0 ]
    [[ "$output" != *"WARNING: lock owner branch resolves"* ]]
    [[ "$output" == *"branch=feat/some-slice"* ]]
}

@test "lock-claim: second claim on same slug returns 1 (lock held)" {
    # First claim — must succeed.
    bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' contended-slug '$WS_FILE'" >/dev/null
    # Second claim from a different clone — must reject.
    CLONE2="$SANDBOX/clone2"
    git clone --quiet "$BARE" "$CLONE2"
    git -C "$CLONE2" config user.email "u@u"
    git -C "$CLONE2" config user.name "u"
    run bash -c "cd '$CLONE2' && bash '$SCRIPTS_DIR/lock-claim.sh' contended-slug '$WS_FILE'"
    [ "$status" -eq 1 ]
    [[ "$output" == *"already held at"* ]]
    [[ "$output" == *"contended-slug"* ]]
}

@test "lock-claim: claim ref contains claim.json with slug + owner" {
    bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' inspect-slug '$WS_FILE'" >/dev/null
    # Read the ref tip's tree blob and inspect claim.json.
    local ref_sha tree_sha blob_sha claim_body
    ref_sha=$(git -C "$CLONE" ls-remote origin refs/locks/inspect-slug | awk '{print $1}')
    [ -n "$ref_sha" ]
    # Need to fetch the commit + read the blob — the clone doesn't have it
    # until fetched.
    git -C "$CLONE" fetch --quiet origin "refs/locks/inspect-slug:refs/locks/inspect-slug"
    tree_sha=$(git -C "$CLONE" cat-file -p "$ref_sha" | awk '/^tree / {print $2}')
    blob_sha=$(git -C "$CLONE" ls-tree "$tree_sha" claim.json | awk '{print $3}')
    claim_body=$(git -C "$CLONE" cat-file -p "$blob_sha")
    # Schema per agents/scripts/core/_lock-json.py build-claim:
    #   slug / owner (= AGENT_ID env) / branch / paths / started / ...
    [[ "$claim_body" == *"\"slug\""* ]]
    [[ "$claim_body" == *"\"inspect-slug\""* ]]
    [[ "$claim_body" == *"\"owner\""* ]]
    [[ "$claim_body" == *"\"bats-test\""* ]]
}

# ---------- Write-set parsing ----------

@test "lock-claim: write-set blank lines + comments stripped" {
    local mixed_ws
    mixed_ws="$SANDBOX/mixed-ws.txt"
    cat > "$mixed_ws" <<'EOF'
# This is a comment
Source/Core/include/Foo.h

# Another comment
Source/Core/src/Foo.cpp

EOF
    run bash -c "cd '$CLONE' && bash '$SCRIPTS_DIR/lock-claim.sh' mixed-ws-slug '$mixed_ws'"
    [ "$status" -eq 0 ]
    [[ "$output" == *"claimed at"* ]]
}
