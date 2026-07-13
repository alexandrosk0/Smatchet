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

    # Isolate the CI-blocker cases from the CodeRabbit-completion gate: default it
    # OFF here so a green rollup with no CodeRabbit row still merges. The dedicated
    # CR tests below flip SAFE_ADMIN_MERGE_CR_INSTALLED=true to exercise the gate.
    export SAFE_ADMIN_MERGE_CR_INSTALLED=false

    # Pin the grace-backstop inputs too: an ambient SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED=true
    # would silently flip BLOCK-path tests to PASS, and a stray _GRACE_MINUTES would
    # perturb the date math. Keep the host environment out of these tests.
    export SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED=false
    unset SAFE_ADMIN_MERGE_CR_GRACE_MINUTES

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

@test "--selftest passes (19/19) and dogfoods the gate" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS — safe-admin-merge --selftest (19/19)"* ]]
}

@test "dedup-to-latest: older CANCELLED run with a newer SUCCESS run reads GREEN (exit 0, merge fires)" {
    # A check name can appear twice when a job is re-run: an older CANCELLED run
    # (e.g. doc-validation cancelled by a body-edit re-trigger) plus a newer
    # SUCCESS run. The guard must dedup to the latest run BEFORE judging red/green,
    # so the stale CANCELLED row does not falsely veto a check whose re-run is
    # green. Required + allow-listed checks all green on their LATEST run.
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"CANCELLED","startedAt":"2026-06-20T10:00:00Z","completedAt":"2026-06-20T10:05:00Z"},
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-20T11:00:00Z","completedAt":"2026-06-20T11:05:00Z"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SENTINEL" ]
}

@test "dedup-to-latest: older SUCCESS with a newer FAILURE still REFUSES (exit 1, no merge)" {
    # Dedup must keep the NEWEST run, not flip a real failure green by picking the
    # wrong one: an older SUCCESS plus a newer FAILURE on the same check blocks.
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"SUCCESS","startedAt":"2026-06-20T10:00:00Z","completedAt":"2026-06-20T10:05:00Z"},
      {"__typename":"CheckRun","name":"Coverage","status":"COMPLETED","conclusion":"FAILURE","startedAt":"2026-06-20T11:00:00Z","completedAt":"2026-06-20T11:05:00Z"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 1 ]
    [[ "$output" == *"Coverage"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "a RED Bucket-C BLOCKS (block-on-any-red flip; was the 2026-06-15 advisory era)" {
    # HISTORY: `Bucket-` was dropped from the allow-list 2026-06-15 while the
    # Mesa-GL lanes could not boot the CI exe, and this test pinned "a RED
    # Bucket-C must NOT block". The all-gates-blocking flip re-arms it as the
    # REFUSES test the old comment promised: the lanes are genuinely green, the
    # blocking scope is every non-advisory-named check, so a red Bucket-C gates
    # the admin-merge like any other check. Required check kept green so
    # Bucket-C is the sole red.
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"CheckRun","name":"Bucket-C (ImGui Test Engine)","status":"COMPLETED","conclusion":"FAILURE"},
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 1 ]
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

@test "an arbitrary non-required RED check BLOCKS (block-on-any-red flip)" {
    # Was "does NOT block" under the curated allow-list era. Under
    # block-on-any-red every non-advisory-named red gates the admin-merge; the
    # advisory-NAME escape keeps its own does-NOT-block test above.
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"some-random-non-required","status":"COMPLETED","conclusion":"FAILURE"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 1 ]
    [[ "$output" == *"some-random-non-required"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
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

@test "REFUSES when a required context is ABSENT from the rollup (exit 1, no merge)" {
    # 'Test-delta gate' is required but no row reports it — a required check
    # missing from the rollup must block, never read as a silent green.
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"}]}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 1 ]
    [[ "$output" == *"Test-delta gate"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "FAILS CLOSED on a malformed rollup that breaks evaluation (exit 2, no merge)" {
    # state parses OPEN but statusCheckRollup is not an array -> evaluate_rollup's
    # jq errors; the call site must fail closed, never silently read zero-blockers.
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":"broken"}'
    run bash "$SCRIPT" 1180
    [ "$status" -eq 2 ]
    [[ "$output" == *"fail-closed"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
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

# ----------------------------------------------------------------------------
# CodeRabbit-completion gate (added 2026-06-17 — the #1332 merge-beats-review race).
# These flip SAFE_ADMIN_MERGE_CR_INSTALLED=true (setup() defaults it OFF) so the
# gate is live. CI rollup is kept fully green in every case, so the ONLY variable
# under test is whether CodeRabbit has completed its review on the head.
# ----------------------------------------------------------------------------

@test "CR gate REFUSES when CodeRabbit has not reviewed the head (exit 1, no merge)" {
    # Green CI, CR installed, but no CodeRabbit row at all — the #1332 race: an
    # admin-merge here would beat the review. Must block.
    export SAFE_ADMIN_MERGE_CR_INSTALLED=true
    export SAFE_ADMIN_MERGE_STUB_ROLLUP="$(green_rollup)"
    run bash "$SCRIPT" 1332
    [ "$status" -eq 1 ]
    [[ "$output" == *"CodeRabbit"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "CR gate ALLOWS when CodeRabbit review is terminal-green on the head (exit 0, merge fires)" {
    export SAFE_ADMIN_MERGE_CR_INSTALLED=true
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"CodeRabbit","status":"COMPLETED","conclusion":"SUCCESS"}]}'
    run bash "$SCRIPT" 1332
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SENTINEL" ]
    run cat "$MERGE_SENTINEL"
    [[ "$output" == *"pr merge 1332 --squash --admin"* ]]
}

@test "CR gate REFUSES while the CodeRabbit review is still in progress (exit 1, no merge)" {
    # CodeRabbit row present but not terminal — review running. Must block, not
    # treat a pending CR CheckRun as a silent pass.
    export SAFE_ADMIN_MERGE_CR_INSTALLED=true
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"},
      {"__typename":"CheckRun","name":"CodeRabbit","status":"IN_PROGRESS","conclusion":null}]}'
    run bash "$SCRIPT" 1332
    [ "$status" -eq 1 ]
    [[ "$output" == *"CodeRabbit"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "CR gate: cr-out-of-band label waives the wait (exit 0, merge fires)" {
    # Explicit operator override — no CodeRabbit row, but the label says merge
    # without waiting. CI stays green; CR is the only thing being waived.
    export SAFE_ADMIN_MERGE_CR_INSTALLED=true
    export SAFE_ADMIN_MERGE_STUB_ROLLUP='{"state":"OPEN","labels":[{"name":"cr-out-of-band"}],"statusCheckRollup":[
      {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS"},
      {"__typename":"StatusContext","context":"Test-delta gate","state":"SUCCESS"}]}'
    run bash "$SCRIPT" 1332
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SENTINEL" ]
}

@test "CR gate: grace expired on a stale head degrades to a backstop pass (exit 0, merge fires)" {
    # CR installed, no review row, but the head is older than the grace window —
    # a CR outage must not wedge every session. Force the grace verdict (test-only).
    export SAFE_ADMIN_MERGE_CR_INSTALLED=true
    export SAFE_ADMIN_MERGE_CR_GRACE_EXPIRED=true
    export SAFE_ADMIN_MERGE_STUB_ROLLUP="$(green_rollup)"
    run bash "$SCRIPT" 1332
    [ "$status" -eq 0 ]
    [ -f "$MERGE_SENTINEL" ]
}

@test "the allow-list is single-sourced from merge-gates.sh (not duplicated)" {
    # The script must NOT carry its own copy of the allow-list regex literal.
    # Pattern narrowed to the live `Coverage|Sanitizer` prefix 2026-06-15 when
    # `Bucket-` was dropped from the canonical regex — the absence-check must
    # match a substring that still exists in the source-of-truth to stay honest.
    run grep -c 'Coverage|Sanitizer' "$SCRIPT"
    [ "$output" -eq 0 ]
    # And the merge-gates source-of-truth must export the shared constant the
    # script reads. It lives in merge-gates.d/00-common.sh (fail-closed-sourced by
    # merge-gates.sh), so search the whole merge-gates.d family, not just the top
    # dispatcher.
    run bash -c "grep -rl 'MERGE_GATES_BLOCK_ALLOWLIST_RE=' '$REPO_ROOT/agents/scripts/core/merge-gates.sh' '$REPO_ROOT/agents/scripts/core/merge-gates.d'"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
}
