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
# ----------------------------------------------------------------------------
# A SECOND wedge class, pinned below (observed on PR #1996, head 51c74fe).
#
# The two invariants above keep the loop ALIVE; they say nothing about the
# verdict it computes. decide() wedged anyway on a head CodeRabbit had fully
# reviewed and marked SUCCESS, because replying to a review thread creates a
# review node with an EMPTY body. That reply satisfied "a CR review exists on
# this head", which skips the StatusContext disambiguation, and then the
# body-parse branch found no "Actionable comments posted:" header in an empty
# body and fail-closed to a retry — every pass, forever. Not a race: nothing
# ever displaces the reply as the latest on-head review, so PENDING was the
# terminal state. Exactly the required-never-terminal class this branch adds
# detection for, in the gate itself.
#
# The fix makes blank-bodied nodes invisible to both selections. These tests run
# the REAL jq programs, extracted from action.yml, against synthetic payloads —
# a copy pasted in here would drift from the action and pass while it broke.
# Both directions are covered, since the dangerous fix is one that clears the
# wedge by waving findings through:
#   - blank reply only            -> not a review     (clears the wedge)
#   - findings + a LATER reply    -> still FAILURE    (no fail-open)
#   - non-blank body, no header   -> still fail-closed (#524 preserved)
#
# Requires: bash, grep, sed, jq, bats. jq is NOT optional and the suite does not
# skip when it is absent: a skipped test reads as a green, which is the failure
# mode this whole branch exists to remove.
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

# ============================================================================
# decide() verdict logic — blank-bodied review nodes must not wedge the gate
# ============================================================================

# Extract a jq program from action.yml rather than restating it, so the tests
# fail if the real expression drifts instead of validating a stale copy.
# Markers are matched with index() as LITERAL substrings — the surrounding shell
# is dense with regex metacharacters ($ ( " \) and an escaping slip here would
# yield an empty program and a vacuously passing suite.
#   extract_jq <start-literal> <end-literal> -> jq program on stdout
extract_jq() {
    awk -v s="$1" -v e="$2" '
        index($0, s) { inblock = 1 }
        inblock      { print }
        inblock && index($0, e) { exit }
    ' "$ACTION" | sed "1s/.*jq -r '//; \$s/' 2>.*//"
}

# payload <reviews-json-array> <cr-status-state> -> fixture file path
payload() {
    local f="$BATS_TEST_TMPDIR/resp.$RANDOM.json"
    cat > "$f" <<EOF
{"data":{"repository":{"pullRequest":{
 "headRefOid":"HEAD",
 "reviews":{"nodes":$1},
 "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
   {"__typename":"StatusContext","context":"CodeRabbit","state":"$2"}]}}}}]}
}}}}
EOF
    printf '%s' "$f"
}

# verdict <fixture> -> one of: not-reviewed | fail-closed | findings:N | clean
# Mirrors decide()'s control flow using the action's own jq + header regex.
verdict() {
    local f="$1" n body num
    n=$(jq -r -f "$BATS_TEST_TMPDIR/count.jq" "$f")
    body=$(jq -r -f "$BATS_TEST_TMPDIR/body.jq" "$f")
    num=$(printf '%s' "$body" | grep -oiE 'Actionable comments posted:[[:space:]]*[0-9]+' \
            | grep -oE '[0-9]+' | head -1 || true)
    if [ "$n" -eq 0 ]; then printf 'not-reviewed'
    elif [ -z "$num" ]; then printf 'fail-closed'
    elif [ "$num" -gt 0 ]; then printf 'findings:%s' "$num"
    else printf 'clean'; fi
}

setup_jq() {
    command -v jq >/dev/null || {
        echo "jq is required by this suite and is not installed" >&2
        return 1
    }
    extract_jq 'n_reviews=$(printf' '|| n_reviews=0' > "$BATS_TEST_TMPDIR/count.jq"
    extract_jq 'body=$(printf'      '|| body=""'     > "$BATS_TEST_TMPDIR/body.jq"
    # An empty extraction would make every assertion below vacuous.
    grep -q 'headRefOid' "$BATS_TEST_TMPDIR/count.jq"
    grep -q 'headRefOid' "$BATS_TEST_TMPDIR/body.jq"
}

CR_NODE='"author":{"login":"coderabbitai[bot]"},"commit":{"oid":"HEAD"}'

@test "verdict: a blank-bodied thread reply is NOT a review (the #1996 wedge)" {
    setup_jq
    # CR replied to a resolved thread on the head and reported SUCCESS. Before
    # the fix this counted as a review, blanked the body, and fail-closed on
    # every pass — PENDING was terminal.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T2\",\"body\":\"\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "not-reviewed" ]   # -> StatusContext SUCCESS -> pass
}

@test "verdict: whitespace-only body is treated as a reply artifact too" {
    setup_jq
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"  \\n\\t \"}]" SUCCESS)"
    [ "$(verdict "$f")" = "not-reviewed" ]
}

@test "verdict: findings survive a LATER blank reply (no fail-open)" {
    setup_jq
    # The dangerous regression: `last` is by submittedAt, so a reply posted
    # after a real review must not blank the body and hide its count.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"**Actionable comments posted: 2**\"},
                   {$CR_NODE,\"submittedAt\":\"T2\",\"body\":\"\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "findings:2" ]
}

@test "verdict: a clean review followed by a blank reply still passes" {
    setup_jq
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"**Actionable comments posted: 0**\"},
                   {$CR_NODE,\"submittedAt\":\"T2\",\"body\":\"\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "clean" ]
}

@test "verdict: non-blank body with no count header stays fail-closed (#524)" {
    setup_jq
    # A real review whose header we cannot find is an unsettled state, not
    # proof of zero findings. It must never reach the StatusContext pass.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"unexpected prose, no header\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "fail-closed" ]
}

@test "selftest: the pre-fix selection reproduces the wedge" {
    setup_jq
    # Strip the blank-body predicate to recreate the old expression; the blank
    # reply must then be counted as a review, which is what wedged the gate.
    sed 's/ *and ((.body \/\/ "") | test("\\\\S"))//' "$BATS_TEST_TMPDIR/count.jq" \
        > "$BATS_TEST_TMPDIR/count.jq.old"
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T2\",\"body\":\"\"}]" SUCCESS)"
    [ "$(jq -r -f "$BATS_TEST_TMPDIR/count.jq.old" "$f")" -eq 1 ]
    [ "$(jq -r -f "$BATS_TEST_TMPDIR/count.jq" "$f")" -eq 0 ]
}
