#!/usr/bin/env bats
# tests/bats/safe_merge.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/safe-merge.sh — the NON-admin merge wrapper
# that runs the full merge-gates poll and arms `gh pr merge --squash --auto`
# ONLY on GATES_PASSED, refuses on any block-allowlist RED check lacking its
# `*-out-of-band` override, and auto-files a tracked deferred-test obligation when
# a load-bearing override rides a strict-zone / trust-boundary diff (PR-1).
#
# The gate decision itself is exercised via SAFE_MERGE_STUB_GATE (PASS/BLOCK/ERROR)
# so no live PR / gh graphql is touched — the merge-gates poll logic has its own
# suite (merge_gates.bats). These tests assert the WRAPPER glue: arm/refuse wiring
# + the obligation-stub side-effect.
#
# Requires: bash, jq (on PATH), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/safe-merge.sh"
    export SCRIPT

    # Per-test scratch dir for obligation stubs so a filed stub never lands in the
    # real backlog tree and is auto-cleaned in teardown.
    OBLIG_DIR="$(mktemp -d)"
    export OBLIG_DIR
    export SAFE_MERGE_OBLIGATION_DIR="$OBLIG_DIR"
    export SAFE_MERGE_OBLIGATION_DATE=2026-06-20

    # Merge-time snapshot ledger — a per-test temp file (the appender's
    # MERGE_SNAPSHOT_LEDGER seam) so a post-arm row never lands in the real
    # ledger. The post-arm merge wait probes once (budget 0) unless a test
    # raises it, so the default stub (PR never reports merged) times out fast.
    SNAP_DIR="$(mktemp -d)"
    export SNAP_DIR
    export MERGE_SNAPSHOT_LEDGER="$SNAP_DIR/merge-snapshots.jsonl"
    export SAFE_MERGE_SNAPSHOT_WAIT_SECONDS=0

    # SAFE_MERGE_STUB_GATE is honoured only in dry-run or explicit test mode;
    # every test here drives a stub gate against the stub gh below.
    export SAFE_MERGE_TEST_MODE=1

    # Stub gh — records `gh pr merge ...` to $MERGE_SENTINEL so a test can assert
    # the merge was (or was NOT) armed; `gh pr merge` exits $STUB_MERGE_RC (default
    # 0). `gh api repos/.../pulls/<n>` (the head reads + the post-arm REST poll)
    # prints $STUB_PR_JSON_AFTER_ARM once the merge was armed (when that file
    # exists), else $STUB_PR_JSON when it exists, else an OPEN unmerged PR at
    # head headsha0. Any other gh call is a no-op success. The gate is stubbed
    # via SAFE_MERGE_STUB_GATE, so `gh api graphql` is never hit.
    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR
    MERGE_SENTINEL="$STUB_BIN_DIR/merge-fired"
    export MERGE_SENTINEL
    export STUB_PR_JSON="$STUB_BIN_DIR/pr.json"
    export STUB_PR_JSON_AFTER_ARM="$STUB_BIN_DIR/pr-after-arm.json"
    cat > "$STUB_BIN_DIR/gh" <<'STUB'
#!/usr/bin/env bash
if [ "$1" = "pr" ] && [ "$2" = "merge" ]; then
    printf '%s\n' "$*" >> "${MERGE_SENTINEL:?}"
    exit "${STUB_MERGE_RC:-0}"
fi
if [ "$1" = "api" ]; then
    case "$2" in
        repos/*/pulls/*)
            if [ -f "${MERGE_SENTINEL:-}" ] && [ -f "${STUB_PR_JSON_AFTER_ARM:-}" ]; then cat "$STUB_PR_JSON_AFTER_ARM"
            elif [ -f "${STUB_PR_JSON:-}" ]; then cat "$STUB_PR_JSON"
            else echo '{"state":"open","merged":false,"merge_commit_sha":"testmerge0","merged_at":null,"head":{"sha":"headsha0"}}'
            fi
            exit 0 ;;
    esac
fi
exit 0
STUB
    chmod +x "$STUB_BIN_DIR/gh"
    export PATH="$STUB_BIN_DIR:$PATH"
}

teardown() {
    rm -rf "$STUB_BIN_DIR" "$OBLIG_DIR" "$SNAP_DIR"
}

# ----------------------------------------------------------------------------

@test "--selftest passes (18/18) and dogfoods arm/refuse/obligation/snapshot" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS — safe-merge --selftest (18/18)"* ]]
}

@test "a leaked SAFE_MERGE_STUB_GATE=PASS outside test mode / dry-run REFUSES (exit 2, nothing armed)" {
    # The stub used to stop implying dry-run; leaked into a real shell it would
    # arm `gh pr merge --squash --auto` with no gate evaluated at all.
    unset SAFE_MERGE_TEST_MODE SAFE_MERGE_DRY_RUN
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_LABELS=""
    run bash "$SCRIPT" 1420
    [ "$status" -eq 2 ]
    [[ "$output" == *"TEST-ONLY seam"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "a stubbed PASS under DRY-RUN (no test mode) still only prints, never arms" {
    unset SAFE_MERGE_TEST_MODE
    export SAFE_MERGE_DRY_RUN=true SAFE_MERGE_STUB_GATE=PASS SAFE_MERGE_LABELS=""
    run bash "$SCRIPT" 1421
    [ "$status" -eq 0 ]
    [[ "$output" == *"DRY-RUN: would run: gh pr merge 1421 --squash --auto --match-head-commit headsha0"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

# ---------- head binding (the arm and the ledger row follow the gated head) ----------

@test "the arm is bound to the gated head with --match-head-commit" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'GATE_HEAD headsha0\nGATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED'
    export SAFE_MERGE_LABELS=""
    run bash "$SCRIPT" 1430
    [ "$status" -eq 0 ]
    grep -q 'pr merge 1430 --squash --auto --match-head-commit headsha0' "$MERGE_SENTINEL"
}

@test "a head that moved after the gate poll REFUSES (exit 1, nothing armed)" {
    # The poll gated gatedsha1; the PR head is now headsha0 — those commits
    # were never gated.
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'GATE_HEAD gatedsha1\nGATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED'
    export SAFE_MERGE_LABELS=""
    run bash "$SCRIPT" 1431
    [ "$status" -eq 1 ]
    [[ "$output" == *"head moved after the gate poll (gated gatedsha1, now headsha0)"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "ledger: a merge that landed at a head the gates never saw writes NO GATES_PASSED row" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'GATE_HEAD headsha0\nGATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED'
    export SAFE_MERGE_LABELS=""
    echo '{"state":"closed","merged":true,"merge_commit_sha":"m9","merged_at":"2026-10-06T10:00:00Z","head":{"sha":"latecommit9"}}' > "$STUB_PR_JSON_AFTER_ARM"
    run bash "$SCRIPT" 1432
    [ "$status" -eq 0 ]
    [[ "$output" == *"merged at head latecommit9, but the gates passed on headsha0"* ]]
    [ ! -s "$MERGE_SNAPSHOT_LEDGER" ]
}

@test "DRY-RUN: a PASS prints the arm command and arms nothing (exit 0)" {
    export SAFE_MERGE_DRY_RUN=true
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT="GATES_PASSED"
    export SAFE_MERGE_LABELS=""
    export SAFE_MERGE_DIFF_PATHS=""
    run bash "$SCRIPT" 1400
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED — PR #1400: arming squash auto-merge"* ]]
    [[ "$output" == *"DRY-RUN: would run: gh pr merge 1400 --squash --auto"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
    [ ! -s "$MERGE_SNAPSHOT_LEDGER" ]
}

@test "a stubbed PASS gate is not a dry-run: it arms through gh (exit 0)" {
    # SAFE_MERGE_STUB_GATE used to force the DRY-RUN exit, which stopped every
    # PASS case one line short of the arm + post-arm ledger write.
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT="GATES_PASSED"
    export SAFE_MERGE_LABELS=""
    run bash "$SCRIPT" 1410
    [ "$status" -eq 0 ]
    [[ "$output" != *"DRY-RUN"* ]]
    grep -q 'pr merge 1410 --squash --auto' "$MERGE_SENTINEL"
}

@test "snapshot: a PASS-gated arm whose merge lands writes exactly one ledger row" {
    # tooling/2026-08-19-safe-merge-arms-automerge-and-execs-away-before-writing-
    # a-snapshot-row: the default merge path used to `exec` gh and write nothing.
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'Poll 1/1 — CI: 4/4 pass (0 fail, 0 pending, 1 warn-downgraded, 0 req-missing) | CodeRabbit: NONE+grace-expired (0 open) | Bugbot: CLEAN (0 open) | User: 0 | reviewDecision: NONE\nGATE_SNAPSHOT cr_override=1 downgraded=Test-delta gate\nGATES_PASSED'
    export SAFE_MERGE_LABELS="tests-out-of-band,cr-out-of-band,area:tooling"
    export SAFE_MERGE_DIFF_PATHS="docs/x.md"
    echo '{"state":"closed","merged":true,"merge_commit_sha":"abc1234def","merged_at":"2026-10-04T10:00:00Z","head":{"sha":"head5678"}}' > "$STUB_PR_JSON"
    run bash "$SCRIPT" 1501
    [ "$status" -eq 0 ]
    [[ "$output" == *"Merge snapshot appended for PR #1501"* ]]
    [ "$(wc -l < "$MERGE_SNAPSHOT_LEDGER")" -eq 1 ]
    [ "$(jq -r '.pr' "$MERGE_SNAPSHOT_LEDGER")" = "1501" ]
    [ "$(jq -r '.mergeActor' "$MERGE_SNAPSHOT_LEDGER")" = "orchestrator-automerge" ]
    [ "$(jq -r '.gates' "$MERGE_SNAPSHOT_LEDGER")" = "GATES_PASSED" ]
    [ "$(jq -r '.mergeCommit' "$MERGE_SNAPSHOT_LEDGER")" = "abc1234def" ]
    [ "$(jq -r '.headSha' "$MERGE_SNAPSHOT_LEDGER")" = "head5678" ]
    [ "$(jq -r '.mergedAt' "$MERGE_SNAPSHOT_LEDGER")" = "2026-10-04T10:00:00Z" ]
    [ "$(jq -r '.redChecks | join("|")' "$MERGE_SNAPSHOT_LEDGER")" = "Test-delta gate|CodeRabbit" ]
    # Only override labels are recorded (area:tooling is not one).
    [ "$(jq -r '.overrideLabels | sort | join("|")' "$MERGE_SNAPSHOT_LEDGER")" = "cr-out-of-band|tests-out-of-band" ]
    # The passing poll's CodeRabbit verdict rides along as crState (schema 3).
    [ "$(jq -r '.crState' "$MERGE_SNAPSHOT_LEDGER")" = "NONE+grace-expired" ]
    [ "$(jq -r '.schema' "$MERGE_SNAPSHOT_LEDGER")" = "3" ]
}

@test "snapshot: a clean pass records empty redChecks and overrideLabels" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'GATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED'
    export SAFE_MERGE_LABELS=""
    echo '{"state":"closed","merged":true,"merge_commit_sha":"c1ea4","merged_at":"2026-10-04T11:00:00Z","head":{"sha":"h1"}}' > "$STUB_PR_JSON"
    run bash "$SCRIPT" 1502
    [ "$status" -eq 0 ]
    [ "$(wc -l < "$MERGE_SNAPSHOT_LEDGER")" -eq 1 ]
    [ "$(jq -r '.redChecks | length' "$MERGE_SNAPSHOT_LEDGER")" = "0" ]
    [ "$(jq -r '.overrideLabels | length' "$MERGE_SNAPSHOT_LEDGER")" = "0" ]
    # No poll line carried a CodeRabbit verdict → no crState, schema stays 2.
    [ "$(jq -r 'has("crState")' "$MERGE_SNAPSHOT_LEDGER")" = "false" ]
    [ "$(jq -r '.schema' "$MERGE_SNAPSHOT_LEDGER")" = "2" ]
}

@test "snapshot: a merge not landed within the budget prints the paste-ready line, writes no row" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'GATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED'
    export SAFE_MERGE_LABELS="perf-out-of-band"
    # Default stub: the PR stays OPEN (auto-merge armed, still queued).
    run bash "$SCRIPT" 1503
    [ "$status" -eq 0 ]
    [[ "$output" == *"SNAPSHOT PENDING — PR #1503"* ]]
    [[ "$output" == *"SNAPSHOT_MERGED_AT=<mergedAt> bash '"*"merge-snapshot-append.sh' 1503 <mergeCommit> headsha0 GATES_PASSED '' 'perf-out-of-band' orchestrator-automerge"* ]]
    [ ! -s "$MERGE_SNAPSHOT_LEDGER" ]
}

@test "snapshot: a PR closed without merging owes no row" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT="GATES_PASSED"
    export SAFE_MERGE_LABELS=""
    echo '{"state":"closed","merged":false,"merge_commit_sha":"x","merged_at":null,"head":{"sha":"h"}}' > "$STUB_PR_JSON"
    run bash "$SCRIPT" 1504
    [ "$status" -eq 0 ]
    [[ "$output" == *"CLOSED without merging"* ]]
    [ ! -s "$MERGE_SNAPSHOT_LEDGER" ]
}

@test "a failed arm passes gh's exit status through and writes no row" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT="GATES_PASSED"
    export SAFE_MERGE_LABELS=""
    export STUB_MERGE_RC=5
    echo '{"state":"closed","merged":true,"merge_commit_sha":"m","merged_at":"2026-10-04T12:00:00Z","head":{"sha":"h"}}' > "$STUB_PR_JSON"
    run bash "$SCRIPT" 1505
    [ "$status" -eq 5 ]
    [[ "$output" == *"auto-merge NOT armed"* ]]
    [ ! -s "$MERGE_SNAPSHOT_LEDGER" ]
}

@test "REFUSES when the gate BLOCKS (exit 1, no merge armed)" {
    export SAFE_MERGE_STUB_GATE=BLOCK
    export SAFE_MERGE_STUB_GATE_OUT="Poll 1/1 — CI: 3/4 pass (1 fail) | blocked"
    run bash "$SCRIPT" 1401
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSED"* ]]
    [[ "$output" == *"did NOT pass"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "REFUSES on a gate-poll precondition error (PR closed or merged, exit 3)" {
    export SAFE_MERGE_STUB_GATE=ERROR
    export SAFE_MERGE_STUB_GATE_OUT="PR_MERGED"
    run bash "$SCRIPT" 1402
    [ "$status" -eq 3 ]
    [[ "$output" == *"precondition error"* ]]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "refuse-when-block-allowlist-gate-red-without-label (gate BLOCK, no merge)" {
    # The block-allowlist refusal is poll_merge_gates' $failing/$downgraded logic:
    # a RED Coverage with NO override label keeps the gate blocked. The wrapper
    # surfaces that as a refuse + no merge. (The full RED-Coverage-without-label
    # vs with-label discrimination lives in merge_gates.bats; here we assert the
    # wrapper REFUSES whenever the gate did not pass and arms NOTHING.)
    export SAFE_MERGE_STUB_GATE=BLOCK
    export SAFE_MERGE_STUB_GATE_OUT="Poll 1/1 — CI: blocked on RED Coverage (no tests-out-of-band)"
    run bash "$SCRIPT" 1403
    [ "$status" -eq 1 ]
    [ ! -f "$MERGE_SENTINEL" ]
}

@test "arm-when-overridden - load-bearing tests-out-of-band over a NON-trust diff arms, no obligation" {
    # The label downgraded a RED check (GATE_SNAPSHOT names it) so the gate PASSES,
    # but the diff is docs-only → no trust boundary → arm with NO obligation stub.
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'Poll 1/1 — passed\nGATE_SNAPSHOT cr_override=0 downgraded=Test-delta gate\nGATES_PASSED'
    export SAFE_MERGE_LABELS="tests-out-of-band"
    export SAFE_MERGE_DIFF_PATHS="docs/x.md README.md"
    run bash "$SCRIPT" 1404
    [ "$status" -eq 0 ]
    [[ "$output" == *"arming squash auto-merge"* ]]
    [ -z "$(ls -A "$OBLIG_DIR" 2>/dev/null)" ]
}

@test "obligation-stub-filed-on-trust-boundary-override (tests-out-of-band + strict-zone diff)" {
    # Load-bearing tests-out-of-band (GATE_SNAPSHOT downgraded Test-delta gate)
    # over a Tracker strict-zone diff → arm AND file a deferred-test obligation.
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'Poll 1/1 — passed\nGATE_SNAPSHOT cr_override=0 downgraded=Test-delta gate\nGATES_PASSED'
    export SAFE_MERGE_LABELS="tests-out-of-band"
    export SAFE_MERGE_DIFF_PATHS="Source/Core/src/Tracker/JiraClient.cpp README.md"
    run bash "$SCRIPT" 1405
    [ "$status" -eq 0 ]
    [[ "$output" == *"OBLIGATION — filed deferred-test stub"* ]]
    # Exactly one stub file landed, naming the PR + the trust-boundary path.
    run bash -c "ls \"$OBLIG_DIR\"/*.md | wc -l"
    [ "$output" -eq 1 ]
    stub="$(ls "$OBLIG_DIR"/*.md)"
    grep -q 'deferred-test obligation' "$stub"
    grep -q 'PR #1405' "$stub"
    grep -q 'Source/Core/src/Tracker/JiraClient.cpp' "$stub"
    grep -q '\[test\]' "$stub"
}

@test "perf-out-of-band over a trust-boundary diff also files an obligation" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'GATE_SNAPSHOT cr_override=0 downgraded=Perf PR-fast (windows-2022)\nGATES_PASSED'
    export SAFE_MERGE_LABELS="perf-out-of-band"
    export SAFE_MERGE_DIFF_PATHS="Source/Core/src/Sync/TicketSyncService.cpp"
    run bash "$SCRIPT" 1406
    [ "$status" -eq 0 ]
    [[ "$output" == *"OBLIGATION"* ]]
    stub="$(ls "$OBLIG_DIR"/*.md)"
    grep -q 'perf-out-of-band' "$stub"
}

@test "a MOOT label (present but downgraded nothing) files NO obligation" {
    # The label is on the PR but the GATE_SNAPSHOT shows it downgraded nothing
    # (the gate passed on its own) — not load-bearing → no obligation, even on a
    # trust-boundary diff. Mirrors the postmortem-owed moot-override filter.
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT=$'GATE_SNAPSHOT cr_override=0 downgraded=\nGATES_PASSED'
    export SAFE_MERGE_LABELS="tests-out-of-band"
    export SAFE_MERGE_DIFF_PATHS="Source/Core/src/Tracker/JiraClient.cpp"
    run bash "$SCRIPT" 1407
    [ "$status" -eq 0 ]
    [[ "$output" != *"OBLIGATION"* ]]
    [ -z "$(ls -A "$OBLIG_DIR" 2>/dev/null)" ]
}

@test "defaults MERGE_GATES_FLIP_READY=true (a draft PR never pauses an authorized merge)" {
    # Under the standing governance.auto_merge grant the orchestrator calls
    # safe-merge on PRs a remote harness may have opened DRAFT. The wrapper is
    # the authorization boundary, so it must default the draft→ready flip on —
    # otherwise CR (auto_review.drafts:false) never reviews and the poll wedges.
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT="GATES_PASSED"
    unset MERGE_GATES_FLIP_READY
    run bash "$SCRIPT" 1408
    [ "$status" -eq 0 ]
    [[ "$output" == *"MERGE_GATES_FLIP_READY defaulted to true"* ]]
    [[ "$output" == *"arming squash auto-merge"* ]]
}

@test "an explicit MERGE_GATES_FLIP_READY=false is honoured (no default override)" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT="GATES_PASSED"
    export MERGE_GATES_FLIP_READY=false
    run bash "$SCRIPT" 1409
    [ "$status" -eq 0 ]
    [[ "$output" != *"defaulted to true"* ]]
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
    # The wrapper must not carry its own copy of the allow-list regex literal —
    # it sources merge-gates.sh and reads MERGE_GATES_BLOCK_ALLOWLIST_RE.
    run grep -c 'Coverage|Sanitizer' "$SCRIPT"
    [ "$output" -eq 0 ]
    run grep -c 'source .*merge-gates.sh' "$SCRIPT"
    [ "$output" -ge 1 ]
}
