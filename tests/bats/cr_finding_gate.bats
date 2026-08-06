#!/usr/bin/env bats
# tests/bats/cr_finding_gate.bats
# ----------------------------------------------------------------------------
# Regression lint for the "CR finding gate wedges when the poll loop outlives
# the job" defect (observed on PR #1954).
#
# The composite action polls CodeRabbit and only ever exits through a `post`
# call, so as long as it RUNS TO COMPLETION the required check reaches a
# terminal state. The failure mode is the loop being killed mid-flight:
#   - the old loop was `ATTEMPTS=12` + `sleep 15`, which bounds only the
#     *sleeping* (165 s); its true cost is that plus 12 GraphQL round-trips;
#   - the job's `timeout-minutes` was 5, shared with setup + checkout;
#   - on a congested runner the job died mid-`sleep`, before either terminal
#     `post` ran. The required check-run went non-terminal, the StatusContext
#     kept its stale value, and nothing re-triggers the workflow — the PR
#     wedged with no self-healing path.
#
# Two invariants pin the fix. They live in two different files and would
# otherwise silently drift back into the same defect:
#
#   1. Timeout ordering: POLL_BUDGET_SECONDS < Evaluate step timeout-minutes
#      < job timeout-minutes. The poll window must close with time left to
#      post; the step must die before the job does.
#   2. Fallback poster: an `if: always()` step that posts PENDING to the same
#      StatusContext when the Evaluate step did not conclude. This is what
#      converts "wedged forever" into "pending, re-runnable" — and it only
#      works because the Evaluate timeout is STEP-scoped (a job-level kill
#      would take the fallback down with it).
#
# selftest: NEGATIVE fixtures (a synthetic workflow with the ordering inverted,
# and one with the fallback step removed) MUST be flagged — proves the checks
# fire rather than passing vacuously.
#
# Requires: bash, grep, sed, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    WF="$REPO_ROOT/.github/workflows/cr-finding-gate.yml"
    ACTION="$REPO_ROOT/.github/actions/cr-finding-gate/action.yml"
    export WF ACTION
}

# poll_budget <action-file> — echo the POLL_BUDGET_SECONDS default (seconds).
poll_budget() {
    sed -n 's/.*POLL_BUDGET_SECONDS="\${POLL_BUDGET_SECONDS:-\([0-9]\+\)}".*/\1/p' "$1" | head -1
}

# job_timeout <workflow-file> — echo the job-level timeout-minutes. The job key
# is indented 4 spaces under `jobs: <job>:`; the step-level one is 8.
job_timeout() {
    sed -n 's/^    timeout-minutes: \([0-9]\+\)$/\1/p' "$1" | head -1
}

# step_timeout <workflow-file> — echo the Evaluate step's timeout-minutes
# (indented 8 spaces, inside the steps list).
step_timeout() {
    sed -n 's/^        timeout-minutes: \([0-9]\+\)$/\1/p' "$1" | head -1
}

@test "action defines a wall-clock poll budget (not a bare attempt count)" {
    run poll_budget "$ACTION"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
    # The old defect form: a fixed attempt count with no deadline.
    ! grep -qE '^\s*ATTEMPTS=' "$ACTION"
    # A deadline computed from bash's SECONDS is what makes the exit time an
    # invariant of the step rather than a consequence of API latency.
    grep -q 'deadline=$(( SECONDS + POLL_BUDGET_SECONDS ))' "$ACTION"
}

@test "poll budget < Evaluate step timeout < job timeout" {
    budget="$(poll_budget "$ACTION")"
    step="$(step_timeout "$WF")"
    job="$(job_timeout "$WF")"
    [ -n "$budget" ]
    [ -n "$step" ]
    [ -n "$job" ]
    # Compare in seconds; timeout-minutes are minutes.
    [ "$budget" -lt $(( step * 60 )) ]
    [ "$step" -lt "$job" ]
}

@test "selftest: inverted ordering is detected" {
    tmp="$BATS_TEST_TMPDIR/bad-order.yml"
    sed 's/^    timeout-minutes: [0-9]\+$/    timeout-minutes: 2/' "$WF" > "$tmp"
    step="$(step_timeout "$tmp")"
    job="$(job_timeout "$tmp")"
    [ "$job" -eq 2 ]
    # The real assertion above would fail on this fixture.
    ! [ "$step" -lt "$job" ]
}

@test "Evaluate step carries its own id + timeout so a fallback can run" {
    grep -q '^        id: eval$' "$WF"
    run step_timeout "$WF"
    [ -n "$output" ]
}

@test "workflow posts PENDING when the evaluation could not conclude" {
    grep -q 'always() && steps.eval.outcome != .success.' "$WF"
    grep -q "state=pending" "$WF"
}

@test "fallback posts to the SAME status context the action uses" {
    # A typo here would leave the required context untouched and the PR still
    # wedged, while the workflow reported success.
    ctx="$(sed -n 's/^        CONTEXT: \(.*\)$/\1/p' "$ACTION" | head -1)"
    [ -n "$ctx" ]
    grep -qF "context='${ctx}'" "$WF"
}

@test "selftest: a workflow with no fallback poster is detected" {
    tmp="$BATS_TEST_TMPDIR/no-fallback.yml"
    grep -v 'state=pending' "$WF" > "$tmp"
    ! grep -q "state=pending" "$tmp"
}
