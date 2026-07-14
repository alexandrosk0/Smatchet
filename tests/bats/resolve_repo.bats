#!/usr/bin/env bats
# resolve_repo.bats — regression suite for the shared repo-slug resolver
# agents/scripts/core/lib/resolve-repo.sh (sourced by setup-branch-protection.sh,
# setup-locks-ruleset.sh, postmortem-owed.sh, plan-archival-owed.sh).
#
# Covers core-scripts-bash-07: the four scripts no longer hardcode
# `alexandrosk0/Smatchet`; they derive the slug via `gh repo view` (or honour a
# $REPO override). The contract under test:
#   1. $REPO set + non-empty       -> echoed verbatim, rc 0 (override + test seam).
#   2. $REPO unset, gh resolves     -> `gh repo view` slug, rc 0.
#   3. $REPO unset, gh absent       -> nothing, rc 1 (NO hardcoded fallback).
#   4. $REPO unset, gh errors        -> nothing, rc 1 (fail-closed, no fallback).
#   5. $REPO unset, gh prints empty  -> nothing, rc 1 (empty slug is not resolved).
#
# resolve_repo calls only `gh` externally (printf/command/[ are bash builtins),
# so each case runs the resolver with PATH set INSIDE the inner shell to exactly
# the dirs under test — the outer `bash` stays discoverable on the bats PATH.
# $LIB is exported in setup() so it survives the narrowed inner PATH.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export LIB="$REPO_ROOT/agents/scripts/core/lib/resolve-repo.sh"
    FAKEBIN="$(mktemp -d)"
    export FAKEBIN
}

teardown() {
    rm -rf "${FAKEBIN:-}"
}

# Install a fake `gh` whose `repo view` prints $1 (default a valid slug) and
# whose exit code is $2 (default 0).
stub_gh() {
    # ${1-…} not ${1:-…}: an explicitly-passed EMPTY slug must stay empty (the
    # "gh prints nothing" case), only a genuinely-omitted arg defaults.
    local slug="${1-acme/widget}" rc="${2:-0}"
    cat > "$FAKEBIN/gh" <<GH
#!/usr/bin/env bash
case "\$*" in
    *"repo view"*) printf '%s\n' "$slug"; exit $rc ;;
    *) exit 0 ;;
esac
GH
    chmod +x "$FAKEBIN/gh"
}

# Inner script: reset PATH to $1 (so only the dirs under test are searched for
# gh), source the lib, run resolve_repo. $LIB comes from the exported env.
INNER='PATH="$1"; . "$LIB"; resolve_repo'

@test "REPO override is echoed verbatim without any gh on PATH (test seam)" {
    # PATH is the empty FAKEBIN (no gh) — the override must not need gh at all.
    REPO="foo/bar" run bash -c "$INNER" _ "$FAKEBIN"
    [ "$status" -eq 0 ]
    [ "$output" = "foo/bar" ]
}

@test "REPO override wins even when gh would resolve to a different slug" {
    stub_gh "other/repo" 0
    REPO="foo/bar" run bash -c "$INNER" _ "$FAKEBIN"
    [ "$status" -eq 0 ]
    [ "$output" = "foo/bar" ]
}

@test "no REPO, gh resolves -> derived slug, rc 0" {
    stub_gh "acme/widget" 0
    run bash -c "unset REPO; $INNER" _ "$FAKEBIN:$PATH"
    [ "$status" -eq 0 ]
    [ "$output" = "acme/widget" ]
}

@test "no REPO, gh absent -> unresolvable (rc 1), NO hardcoded fallback" {
    # Empty FAKEBIN: no gh discoverable.
    run bash -c "unset REPO; $INNER" _ "$FAKEBIN"
    [ "$status" -eq 1 ]
    [ -z "$output" ]
    [[ "$output" != *"alexandrosk0"* ]]
}

@test "no REPO, gh errors -> unresolvable (rc 1)" {
    stub_gh "acme/widget" 1
    run bash -c "unset REPO; $INNER" _ "$FAKEBIN:$PATH"
    [ "$status" -eq 1 ]
    [ -z "$output" ]
}

@test "no REPO, gh prints empty slug -> unresolvable (rc 1)" {
    stub_gh "" 0
    run bash -c "unset REPO; $INNER" _ "$FAKEBIN:$PATH"
    [ "$status" -eq 1 ]
    [ -z "$output" ]
}

@test "empty REPO (set but blank) falls through to gh, not echoed" {
    stub_gh "acme/widget" 0
    REPO="" run bash -c "$INNER" _ "$FAKEBIN:$PATH"
    [ "$status" -eq 0 ]
    [ "$output" = "acme/widget" ]
}
