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

# --- blocking-awareness: only a check that actually GATES merge fires RED -------

@test "--once does NOT RED an advisory-NAMED check failure (the block-on-any-red escape)" {
    # Under block-on-any-red every non-required red gates EXCEPT a check whose
    # name carries the "advisory" token (no production lane does; the fixture
    # name below keeps the retired suffix precisely to exercise the escape).
    _stub_gh '{"state":"OPEN","labels":[],"statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"C++ lint (catch-all + cppcheck, advisory)","status":"COMPLETED","conclusion":"FAILURE"}],"reviews":[{"author":{"login":"coderabbitai"},"state":"APPROVED","body":""}]}'
    run env PATH="$STUBDIR:$PATH" PR_STATUS_WATCH_REQUIRED_CONTEXTS=$'Windows + MSVC\nTest-delta gate' bash "$SCRIPT" --once 21
    [ "$status" -eq 0 ]
    [[ "$output" != *"RED"* ]]
    [[ "$output" == *"PR #21 all green + CR clear"* ]]
}

@test "--once REDs an allow-listed non-advisory failure even when non-required (Bucket-)" {
    _stub_gh '{"state":"OPEN","labels":[],"statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    run env PATH="$STUBDIR:$PATH" PR_STATUS_WATCH_REQUIRED_CONTEXTS=$'Windows + MSVC\nTest-delta gate' bash "$SCRIPT" --once 22
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #22 RED: Bucket-C (ImGui Test Engine)"* ]]
}

@test "--once does NOT RED a perf-out-of-band-downgraded Perf PR-fast failure" {
    _stub_gh '{"state":"OPEN","labels":[{"name":"perf-out-of-band"}],"statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"FAILURE"}],"reviews":[{"author":{"login":"coderabbitai"},"state":"APPROVED","body":""}]}'
    run env PATH="$STUBDIR:$PATH" PR_STATUS_WATCH_REQUIRED_CONTEXTS=$'Windows + MSVC\nTest-delta gate' bash "$SCRIPT" --once 23
    [ "$status" -eq 0 ]
    [[ "$output" != *"RED"* ]]
}

@test "--once does NOT RED a tests-out-of-band-downgraded Test-delta gate failure" {
    _stub_gh '{"state":"OPEN","labels":[{"name":"tests-out-of-band"}],"statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"Test-delta gate","status":"COMPLETED","conclusion":"FAILURE"}],"reviews":[{"author":{"login":"coderabbitai"},"state":"APPROVED","body":""}]}'
    run env PATH="$STUBDIR:$PATH" PR_STATUS_WATCH_REQUIRED_CONTEXTS=$'Windows + MSVC\nTest-delta gate' bash "$SCRIPT" --once 24
    [ "$status" -eq 0 ]
    [[ "$output" != *"RED"* ]]
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

@test "--once flags CI-green-but-CodeRabbit-actionable when the review is on the current head" {
    _stub_gh '{"state":"OPEN","headRefOid":"HEADSHA","statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"CodeRabbit","status":"IN_PROGRESS","conclusion":null}],"reviews":[{"author":{"login":"coderabbitai"},"state":"COMMENTED","commit":{"oid":"HEADSHA"},"body":"**Actionable comments posted: 2**"}]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 11
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR #11 CI green but CodeRabbit has 2 actionable finding(s) — blocked on review"* ]]
}

@test "--once SUPPRESSES a stale CR count when the review is on a pre-fix-push commit" {
    _stub_gh '{"state":"OPEN","headRefOid":"NEWSHA","statusCheckRollup":[{"name":"Windows + MSVC","status":"COMPLETED","conclusion":"SUCCESS"},{"name":"CodeRabbit","status":"IN_PROGRESS","conclusion":null}],"reviews":[{"author":{"login":"coderabbitai"},"state":"COMMENTED","commit":{"oid":"OLDSHA"},"body":"**Actionable comments posted: 2**"}]}'
    run env PATH="$STUBDIR:$PATH" bash "$SCRIPT" --once 13
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR #13"* ]]
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
