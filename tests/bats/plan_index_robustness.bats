#!/usr/bin/env bats
# tests/bats/plan_index_robustness.bats
# ----------------------------------------------------------------------------
# Bats coverage for the PR-7 plan-index robustness work:
#   * agents/scripts/core/lib/plan-history-guard.sh — is_shallow_or_refuse()
#     (shallow-clone-corrupts-git-log-date-generators /
#      test-plan-index-shallow-clone-corrupts-date-sort)
#   * agents/scripts/core/test-plan-index.sh — git-root path resolution so a
#     subdir/worktree --fix can't silent-NO-OP (plan-index-fix-wrong-cwd-silent-noop)
#
# Strategy: build a throwaway "upstream" repo with full history + a shipped
# plan, then `git clone --depth 1` it to a SHALLOW working copy. The shallow
# copy's `origin` is the local upstream path, so an auto-unshallow can actually
# succeed; we break `origin` to exercise the hard-refuse branch.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export GUARD_LIB="$REPO_ROOT/agents/scripts/core/lib/plan-history-guard.sh"
    export PLAN_INDEX="$REPO_ROOT/agents/scripts/core/test-plan-index.sh"

    SANDBOX="$(mktemp -d)"
    export SANDBOX
    export UPSTREAM="$SANDBOX/upstream"
    export SHALLOW="$SANDBOX/shallow"

    # Upstream: a few commits so a shallow clone is genuinely truncated.
    # Build the shipped-plan path via vars so the literal substring
    # "docs/plans/shipped/<name>.md" never appears verbatim in this file — that
    # exact form would trip test-plan-ref-integrity's repo-wide git-grep (it
    # extracts the path text regardless of any prefix) into flagging this fixture
    # path as a dangling real-tree reference.
    local shipdir="docs/plans/shipped"
    local plan="$shipdir/alpha.md"
    git init --quiet -b develop "$UPSTREAM"
    git -C "$UPSTREAM" config user.email t@t
    git -C "$UPSTREAM" config user.name t
    mkdir -p "$UPSTREAM/$shipdir"
    printf '# Seed\n' > "$UPSTREAM/seed.md"
    git -C "$UPSTREAM" add -A && git -C "$UPSTREAM" commit --quiet -m seed
    printf '# A shipped plan\n' > "$UPSTREAM/$plan"
    git -C "$UPSTREAM" add -A && git -C "$UPSTREAM" commit --quiet -m "add alpha plan"
    printf '# A shipped plan\n\nmore\n' > "$UPSTREAM/$plan"
    git -C "$UPSTREAM" add -A && git -C "$UPSTREAM" commit --quiet -m "edit alpha plan"
}

teardown() {
    rm -rf "${SANDBOX:-}"
}

# Helper: clone UPSTREAM shallow into SHALLOW.
_make_shallow() {
    git clone --quiet --depth 1 "file://$UPSTREAM" "$SHALLOW" >/dev/null 2>&1
    [ "$(git -C "$SHALLOW" rev-parse --is-shallow-repository)" = "true" ]
}

# ---------- guard: non-shallow is a no-op ----------

@test "is_shallow_or_refuse: non-shallow repo returns 0 in both modes" {
    # The live PR-7 repo is non-shallow; source the lib and call directly.
    run bash -c ". '$GUARD_LIB'; is_shallow_or_refuse fix && is_shallow_or_refuse check && echo OK"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

# ---------- guard: shallow + check = WARN, never refuse ----------

@test "is_shallow_or_refuse: shallow + check WARNs and exits 0 (read-only)" {
    _make_shallow
    run bash -c "cd '$SHALLOW' && . '$GUARD_LIB'; is_shallow_or_refuse check; echo rc=\$?"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"shallow"* ]]
    [[ "$output" == *"rc=0"* ]]
}

# ---------- guard: shallow + fix = auto-unshallow when origin reachable ----------

@test "is_shallow_or_refuse: shallow + fix auto-unshallows when origin is reachable" {
    _make_shallow
    run bash -c "cd '$SHALLOW' && . '$GUARD_LIB'; is_shallow_or_refuse fix; echo rc=\$?"
    [ "$status" -eq 0 ]
    [[ "$output" == *"auto-unshallow"* ]]
    [[ "$output" == *"rc=0"* ]]
    # And the working copy is no longer shallow.
    [ "$(git -C "$SHALLOW" rev-parse --is-shallow-repository)" = "false" ]
}

# ---------- guard: shallow + fix = hard-refuse (exit 2) when unshallow fails ----------

@test "is_shallow_or_refuse: shallow + fix hard-refuses (exit 2) when origin unreachable" {
    _make_shallow
    # Break origin so `git fetch --unshallow` fails -> the refuse branch.
    git -C "$SHALLOW" remote set-url origin "file://$SANDBOX/does-not-exist"
    run bash -c "cd '$SHALLOW' && . '$GUARD_LIB'; is_shallow_or_refuse fix; echo rc=\$?"
    # is_shallow_or_refuse calls `exit 2` directly on refuse.
    [ "$status" -eq 2 ]
    [[ "$output" == *"REFUSING"* ]]
    [[ "$output" == *"unshallow"* ]]
    # The 'rc=' echo must NOT run — exit aborted the subshell.
    [[ "$output" != *"rc="* ]]
}

# ---------- wrong-cwd: --selftest resolves ARCHIVE_DIR against git root ----------

@test "test-plan-index --selftest passes from the repo root" {
    run bash "$PLAN_INDEX" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

@test "test-plan-index --selftest passes when invoked from a SUBDIR (git-root cd)" {
    # The wrong-cwd bug: invoked from a subdir, a relative ARCHIVE_DIR would not
    # resolve and --fix would silent-NO-OP. The cd-to-toplevel fix means the
    # selftest (which asserts ARCHIVE_DIR exists after the cd) passes anywhere.
    sub="$REPO_ROOT/agents/scripts/core/lib"
    run bash -c "cd '$sub' && bash '$PLAN_INDEX' --selftest"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}
