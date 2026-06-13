#!/usr/bin/env bats
# tests/bats/safe_admin_merge.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/safe-admin-merge.sh — the admin-merge guard
# that makes the green assertion an EXIT CODE (preventing gate for the #1180
# escape, postmortems.md 2026-06-13).
#
# Stubs `gh` on PATH (records the merge invocation to a sentinel file) and feeds
# synthetic rollups via SAFE_ADMIN_MERGE_STUB_ROLLUP so no live PR is touched.
# Requires: bash, jq (on PATH), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/safe-admin-merge.sh"
    export SCRIPT

    # Fixed required-context set so tests are independent of project.config.json.
    export SAFE_ADMIN_MERGE_REQUIRED_CONTEXTS=$'Windows + MSVC\nTest-delta gate'

    # Stub gh — records `gh pr merge ...` to $MERGE_SENTINEL so a test can assert
    # the merge fired (or did NOT). Any other gh call is a no-op success.
    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR
    MERGE_SENTINEL="$STUB_BIN_DIR/merge-fired"
    export MERGE_SENTINEL
    cat > "$STUB_BIN_DIR/gh" <<'STUB'
#!/usr/bin/env bash
if [ "$1" = "pr" ] && [ "$2" = "merge" ]; then
    printf '%s\n' "$*" >> "${MERGE_SENTINEL:?}"
    exit 0
fi
# `gh pr view` should never be reached when SAFE_ADMIN_MERGE_STUB_ROLLUP is set;
# fail loudly if it is, so a test that forgets the stub can't pass silently.
if [ "$1" = "pr" ] && [ "$2" = "view" ]; then
    echo "stub gh: unexpected 'gh pr view' (rollup stub not set)" >&2
    exit 1
fi
exit 0
STUB
    chmod +x "$STUB_BIN_DIR/gh"
    export PATH="$STUB_BIN_DIR:$PATH"
}

teardown() {
    rm -rf "$STUB_BIN_DIR"
}

# A green rollup: every required + allow-listed check SUCCESS.
green_rollup() {
    cat <<'JSON'
{"state":"OPEN","labels":[],"statusCheckRollup":[
  {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
  {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
  {"__typename":"CheckRun","name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"SUCCESS"},
  {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS"}]}
JSON
}

# ----------------------------------------------------------------------------

@test "--selftest passes (4/4) and dogfoods the gate" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS — safe-admin-merge --selftest (4/4)"* ]]
}

@test "REFUSES on a RED allow-listed Bucket check (exit 1, no merge)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"FAILURE"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSED"* ]]
    [[ "$output" == *"Bucket-C"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "REFUSES on a RED required StatusContext (exit 1, no merge)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"FAILURE"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 1 ]
    [[ "$output" == *"Windows + MSVC"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "REFUSES on a PENDING allow-listed check (exit 1, no merge)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Sanitizer","status":"IN_PROGRESS","conclusion":null},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 1 ]
    [[ "$output" == *"Sanitizer"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "ALLOWS a stale-BLOCKED-all-green PR and fires the admin-merge (exit 0)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP="$(green_rollup)"
    run bash "$SCRIPT" 1180
    [ "$status" -eq 0 ]
    [[ "$output" == *"GREEN"* ]]
    [ -f "$MERGE_SENTINEL" ]
    run cat "$MERGE_SENTINEL"
    [[ "$output" == *"pr merge 1180 --squash --admin"* ]]
}

@test "an advisory RED check does NOT block (exit 0, merge fires)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"Duplication scanner (advisory)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SENTINEL" ]
}

@test "a non-required non-allow-listed RED check does NOT block (exit 0)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"some-random-non-required","status":"COMPLETED","conclusion":"FAILURE"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SENTINEL" ]
}

@test "perf-out-of-band downgrades a RED Perf PR-fast (exit 0, merge fires)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[{"name":"perf-out-of-band"}],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"FAILURE"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SENTINEL" ]
}

@test "DRY-RUN prints the merge command but does NOT fire it" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP="$(green_rollup)"
    export SAFE_ADMIN_MERGE_DRY_RUN=true
    run bash "$SCRIPT" 1180
    [ "$status" -eq 0 ]
    [[ "$output" == *"DRY-RUN: would run: gh pr merge 1180 --squash --admin"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "REFUSES a non-OPEN PR (exit 3, no merge)" {
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"MERGED","labels":[],"statusCheckRollup":[]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 3 ]
    [[ "$output" == *"not OPEN"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "rejects a non-numeric PR arg (exit 2)" {
    run bash "$SCRIPT" not-a-number
    [ "$status" -eq 2 ]
    [[ "$output" == *"must be a PR number"* ]]
}

@test "no arg prints usage (exit 2)" {
    run bash "$SCRIPT"
    [ "$status" -eq 2 ]
}

@test "the allow-list is single-sourced from merge-gates.sh (not duplicated)" {
    # The script must NOT carry its own copy of the allow-list regex literal.
    run grep -c 'Coverage|Sanitizer|Bucket-' "$SCRIPT"
    [ "$output" -eq 0 ]
    # And merge-gates.sh must export the shared constant the script reads.
    run grep -c 'MERGE_GATES_BLOCK_ALLOWLIST_RE=' "$REPO_ROOT/agents/scripts/core/merge-gates.sh"
    [ "$output" -ge 1 ]
}
