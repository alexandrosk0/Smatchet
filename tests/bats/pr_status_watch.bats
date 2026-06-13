#!/usr/bin/env bats
# tests/bats/pr_status_watch.bats
# ----------------------------------------------------------------------------
# Bats coverage for scripts/dev/pr-status-watch.sh — the per-PR notify-on-state
# helper that closes the merge-watcher-is-silent blind spot.
#
# Covers: the --selftest classifier, usage error, and --once over a STUBBED `gh`
# (canned statusCheckRollup JSON on PATH) for RED / MERGED / green-stuck / pending.
# Requires: bash, bats, jq.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export SCRIPT="$REPO_ROOT/scripts/dev/pr-status-watch.sh"
    STUBDIR="$(mktemp -d)"
    export STUBDIR
}

teardown() {
    [ -n "${STUBDIR:-}" ] && rm -rf "$STUBDIR"
}

# Write a fake `gh` to $STUBDIR that echoes the given JSON for any `gh pr view`.
_stub_gh() { # $1 = json
    cat > "$STUBDIR/gh" <<STUB
#!/usr/bin/env bash
cat <<'JSON'
$1
JSON
STUB
    chmod +x "$STUBDIR/gh"
}

@test "--selftest passes (classifier fixtures incl. asserts-failure RED)" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"classifier OK"* ]]
}

@test "no PR args is a usage error (exit 2)" {
    run bash "$SCRIPT"
    [ "$status" -eq 2 ]
}

@test "--once emits RED for a failing required check" {
    _stub_gh '{"state":"OPEN","statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"FAILURE"},{"name":"Shell lint","status":"COMPLETED","conclusion":"SUCCESS"}]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 999
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #999 RED: Windows + MSVC"* ]]
}

@test "--once emits MERGED" {
    _stub_gh '{"state":"MERGED","statusCheckRollup":[]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 42
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #42 MERGED"* ]]
}

@test "--once flags green + CR clear (CI green, CR approved, still OPEN)" {
    _stub_gh '{"state":"OPEN","statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"}],"reviews":[{"author":{"login":"coderabbitai"},"state":"APPROVED","body":""}]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 7
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #7 all green + CR clear — awaiting watcher merge"* ]]
}

@test "--once flags CI-green-but-CodeRabbit-actionable (the blocked-on-review state)" {
    _stub_gh '{"state":"OPEN","statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"CodeRabbit","status":"IN_PROGRESS","conclusion":null}],"reviews":[{"author":{"login":"coderabbitai"},"state":"COMMENTED","body":"**Actionable comments posted: 2**"}]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 11
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #11 CI green but CodeRabbit has 2 actionable finding(s) — blocked on review"* ]]
}

@test "--once stays silent when CI green but CR review not yet posted" {
    _stub_gh '{"state":"OPEN","statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"CodeRabbit","status":"IN_PROGRESS","conclusion":null}],"reviews":[]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 12
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #12"* ]]
}

@test "--once stays silent while a check is still pending" {
    _stub_gh '{"state":"OPEN","statusCheckRollup":[{"name":"Windows + MSVC","status":"IN_PROGRESS","conclusion":null}]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 8
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #8"* ]]
}
