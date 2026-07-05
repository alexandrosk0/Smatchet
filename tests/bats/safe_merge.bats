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

    # Stub gh — records `gh pr merge ...` to $MERGE_SENTINEL so a test can assert
    # the merge was (or was NOT) armed. Any other gh call is a no-op success. The
    # gate is stubbed via SAFE_MERGE_STUB_GATE, so `gh api graphql` is never hit.
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
exit 0
STUB
    chmod +x "$STUB_BIN_DIR/gh"
    export PATH="$STUB_BIN_DIR:$PATH"
}

teardown() {
    rm -rf "$STUB_BIN_DIR" "$OBLIG_DIR"
}

# ----------------------------------------------------------------------------

@test "--selftest passes (13/13) and dogfoods arm/refuse/obligation" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS — safe-merge --selftest (13/13)"* ]]
}

@test "arms auto-merge when the gate PASSES (exit 0)" {
    export SAFE_MERGE_STUB_GATE=PASS
    export SAFE_MERGE_STUB_GATE_OUT="GATES_PASSED"
    export SAFE_MERGE_LABELS=""
    export SAFE_MERGE_DIFF_PATHS=""
    run bash "$SCRIPT" 1400
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED — PR #1400: arming squash auto-merge"* ]]
    [[ "$output" == *"DRY-RUN: would run: gh pr merge 1400 --squash --auto"* ]]
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
