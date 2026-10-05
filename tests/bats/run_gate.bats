#!/usr/bin/env bats
# tests/bats/run_gate.bats
# ----------------------------------------------------------------------------
# agents/scripts/core/run-gate.sh — the one correct invocation shape for a command
# whose exit code IS the verdict (process 2026-08-18
# piping-a-gate-into-tail-masks-its-exit-code). `gate | tail -N` reports tail's
# status and drops the failure identities; run-gate.sh captures the full output to
# a log, stamps `<NAME>_EXIT=<rc>`, prints the verdict lines, and exits with the
# gate's own rc.
#
# Requires: bash, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    RUN_GATE="$REPO_ROOT/agents/scripts/core/run-gate.sh"
    tmp="$(mktemp -d)"
    # A fake gate: chatty output, a failure identity, a summary tally, exit <rc>.
    cat > "$tmp/test-fake-suite.sh" <<'EOF'
#!/usr/bin/env bash
for i in $(seq 1 40); do echo "progress line $i"; done
echo "case alpha: PASS"
echo "case beta: FAIL (expected 3 got 4)"
echo "AGGREGATE  Passed: 1  Failed: 36"
exit "${FAKE_RC:-0}"
EOF
}

teardown() {
    [ -n "${tmp:-}" ] && rm -rf "$tmp"
    return 0
}

@test "run-gate: a failing gate's rc propagates where a tail pipe reports 0" {
    # The masked form this helper replaces: tail's status, not the gate's.
    run bash -c "FAKE_RC=1 bash '$tmp/test-fake-suite.sh' 2>&1 | tail -1"
    [ "$status" -eq 0 ]
    FAKE_RC=1 run bash "$RUN_GATE" "$tmp/out.log" -- bash "$tmp/test-fake-suite.sh"
    [ "$status" -eq 1 ]
    [[ "$output" == *"TEST_FAKE_SUITE_EXIT=1"* ]]
    [[ "$output" == *"case beta: FAIL (expected 3 got 4)"* ]]
    [[ "$output" == *"Passed: 1  Failed: 36"* ]]
    [[ "$output" != *"progress line"* ]]
}

@test "run-gate: the log holds the FULL output plus the EXIT stamp" {
    FAKE_RC=2 run bash "$RUN_GATE" "$tmp/nested/dir/out.log" -- bash "$tmp/test-fake-suite.sh"
    [ "$status" -eq 2 ]
    [ "$(grep -c '^progress line' "$tmp/nested/dir/out.log")" -eq 40 ]
    [ "$(tail -n 1 "$tmp/nested/dir/out.log")" = "TEST_FAKE_SUITE_EXIT=2" ]
}

@test "run-gate: a passing gate exits 0 and reports its PASS line" {
    run bash "$RUN_GATE" "$tmp/out.log" -- bash "$tmp/test-fake-suite.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"TEST_FAKE_SUITE_EXIT=0"* ]]
    [[ "$output" == *"case alpha: PASS"* ]]
}

@test "run-gate: NAME skips env, VAR=value and interpreter words; --name overrides" {
    run bash "$RUN_GATE" "$tmp/out.log" -- env FAKE_RC=0 bash "$tmp/test-fake-suite.sh"
    [[ "$output" == *"TEST_FAKE_SUITE_EXIT=0"* ]]
    run bash "$RUN_GATE" --name check-pr-intent "$tmp/out.log" -- true
    [ "$status" -eq 0 ]
    [[ "$output" == *"CHECK_PR_INTENT_EXIT=0"* ]]
    [[ "$output" == *"no verdict lines matched"* ]]
}

@test "run-gate: verdict lines are capped with a pointer to the full log" {
    printf 'echo "FAIL row %s"\n' $(seq 1 30) > "$tmp/many.sh"
    printf 'exit 1\n' >> "$tmp/many.sh"
    RUN_GATE_MAX_LINES=5 run bash "$RUN_GATE" "$tmp/out.log" -- bash "$tmp/many.sh"
    [ "$status" -eq 1 ]
    [ "$(printf '%s\n' "$output" | grep -c '^FAIL row')" -eq 5 ]
    [[ "$output" == *"25 more verdict line(s)"* ]]
}

@test "run-gate: an unknown command is rc 127, not a pass" {
    # rc echoed from inside the subshell: bats `run` warns (BW01) on a bare 127.
    run bash -c "bash '$RUN_GATE' '$tmp/out.log' -- '$tmp/does-not-exist.sh'; echo \"rc=\$?\""
    [[ "$output" == *"DOES_NOT_EXIST_EXIT=127"* ]]
    [[ "$output" == *"rc=127"* ]]
}

@test "run-gate: missing -- separator or command is a usage error (rc 2)" {
    run bash "$RUN_GATE" "$tmp/out.log" bash "$tmp/test-fake-suite.sh"
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
    run bash "$RUN_GATE" "$tmp/out.log" --
    [ "$status" -eq 2 ]
}
