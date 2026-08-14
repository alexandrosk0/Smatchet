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

@test "workflow has NO concurrency block (pending-queue collapse stays gone)" {
    # A per-PR concurrency group with cancel-in-progress:false still lets
    # GitHub cancel PENDING runs (one pending per group), and a run cancelled
    # before it starts creates no check-run — the required context goes
    # absent-forever (#1937; cr-finding-gate-pending-queue-collapse entry).
    # Every run posts a status computed from current head state, so parallel
    # runs converge and no concurrency limiting is needed or safe here.
    # Match YAML keys only — the explanatory comment in the workflow names
    # cancel-in-progress in prose, which must not trip this pin.
    ! grep -qE '^concurrency:' "$WF"
    ! grep -qE '^[[:space:]]+cancel-in-progress:' "$WF"
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

# payload <reviews-json-array> <cr-status-state> [cr-status-description]
#   -> fixture file path. Description defaults to CR's real settled wording.
payload() {
    local f="$BATS_TEST_TMPDIR/resp.$RANDOM.json"
    cat > "$f" <<EOF
{"data":{"repository":{"pullRequest":{
 "headRefOid":"HEAD",
 "reviews":{"nodes":$1},
 "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
   {"__typename":"StatusContext","context":"CodeRabbit","state":"$2",
    "description":"${3:-Review completed}"}]}}}}]}
}}}}
EOF
    printf '%s' "$f"
}

# The not-a-review marker decide() refuses to treat as review evidence. Restated
# here for readability, then asserted to still exist in the action verbatim (see
# the "guard is present" test) so this cannot drift into testing a pattern the
# gate no longer uses.
RATE_LIMIT_RE='rate.?limit|limit reached'

# The positive half of the shape-3 discrimination: a header-less on-head
# review resolves clean ONLY when the observed clean-with-nitpicks shape is
# present (2026-08-12 entry) — generic furniture ("Review details" etc.)
# appears in every CR body and proves nothing. The negative half (no line may
# pair "actionable" with a digit, catching a format-drifted count header) is
# mirrored inside verdict() below. Same restate-then-assert-verbatim contract
# as RATE_LIMIT_RE above.
NITPICK_CLEAN_RE='nitpick comments'

# verdict <fixture> -> not-reviewed | unsettled | fail-closed | findings:N | clean
# Mirrors decide()'s control flow using the action's own jq + header regex.
#   not-reviewed = no verdict-carrying review, CR status SUCCESS -> gate passes
#   unsettled    = retry -> PENDING at window exhaustion (never green)
verdict() {
    local f="$1" n body num state desc
    n=$(jq -r -f "$BATS_TEST_TMPDIR/count.jq" "$f")
    if [ "$n" -eq 0 ]; then
        state=$(jq -r -f "$BATS_TEST_TMPDIR/ctx.jq" "$f")
        desc=$(jq -r -f "$BATS_TEST_TMPDIR/desc.jq" "$f")
        if [ "$state" = "SUCCESS" ] && printf '%s' "$desc" | grep -qiE "$RATE_LIMIT_RE"; then
            printf 'unsettled'; return
        fi
        case "$state" in
            SUCCESS)       printf 'not-reviewed' ;;
            FAILURE|ERROR) printf 'cr-failed' ;;
            *)             printf 'unsettled' ;;
        esac
        return
    fi
    body=$(jq -r -f "$BATS_TEST_TMPDIR/body.jq" "$f")
    num=$(printf '%s' "$body" | grep -oiE 'Actionable comments posted:[[:space:]]*[0-9]+' \
            | grep -oE '[0-9]+' | head -1 || true)
    if [ -z "$num" ]; then
        if printf '%s' "$body" | grep -qiE "$NITPICK_CLEAN_RE" \
           && ! printf '%s' "$body" | grep -i 'actionable' | grep -q '[0-9]'; then
            printf 'nitpick-clean'
        else printf 'fail-closed'; fi
    elif [ "$num" -gt 0 ]; then printf 'findings:%s' "$num"
    else printf 'clean'; fi
}

setup_jq() {
    command -v jq >/dev/null || {
        echo "jq is required by this suite and is not installed" >&2
        return 1
    }
    extract_jq 'n_reviews=$(printf' '|| n_reviews=0'    > "$BATS_TEST_TMPDIR/count.jq"
    extract_jq 'body=$(printf'      '|| body=""'        > "$BATS_TEST_TMPDIR/body.jq"
    extract_jq 'cr_ctx=$(printf'    '|| cr_ctx="ABSENT"' > "$BATS_TEST_TMPDIR/ctx.jq"
    extract_jq 'cr_desc=$(printf'   '|| cr_desc=""'      > "$BATS_TEST_TMPDIR/desc.jq"
    # An empty extraction would make every assertion below vacuous.
    grep -q 'headRefOid' "$BATS_TEST_TMPDIR/count.jq"
    grep -q 'headRefOid' "$BATS_TEST_TMPDIR/body.jq"
    # `StatusContext` alone cannot tell ctx.jq and desc.jq apart — both select it.
    # If the extraction markers drifted and desc.jq came back as the STATE program,
    # the vacuity check would still pass while the rate-limit tests silently
    # compared state against state. Assert the distinguishing selector in each.
    grep -q 'StatusContext' "$BATS_TEST_TMPDIR/ctx.jq"
    grep -q '\.state'       "$BATS_TEST_TMPDIR/ctx.jq"
    grep -q 'StatusContext' "$BATS_TEST_TMPDIR/desc.jq"
    grep -q '\.description' "$BATS_TEST_TMPDIR/desc.jq"
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
    # A review whose header-less body shows NONE of CR's review furniture is an
    # unsettled state, not proof of zero findings. It must never reach the
    # StatusContext pass. (Header-less bodies WITH furniture resolve clean —
    # the shape-3 tests below — so this fixture is deliberately bare prose.)
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"unexpected prose, no header\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "fail-closed" ]
}

@test "verdict: header-less clean-with-nitpicks review resolves clean (shape 3)" {
    setup_jq
    # The #2002 round-12 wedge: a full review submitted a review object on the
    # current head whose clean-pass body carried only nitpicks — no "Actionable
    # comments posted: N" line. Fail-closing here is a PERMANENT wedge, because
    # the review being retried for already happened and a re-requested full
    # review is clean again (header-less again). The nitpick section is CR
    # review furniture -> settled clean verdict.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"🧹 Nitpick comments (2) … prose …\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "nitpick-clean" ]
}

@test "verdict: header-less body with only generic furniture stays fail-closed" {
    setup_jq
    # "Review details" / the auto-generated marker appear in EVERY CR body,
    # findings included. Accepting them alone would let a format-drifted
    # findings header go green — the positive signal must be the nitpick
    # section specifically.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"Review details… <!-- This is an auto-generated comment by CodeRabbit for review status -->\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "fail-closed" ]
}

@test "verdict: a drifted digit-bearing actionable line blocks the shape-3 pass" {
    setup_jq
    # The symmetric fail-open: CR renames the header (e.g. drops 'posted') so
    # the count parse misses, but the body still pairs 'actionable' with a
    # digit — that is findings-shaped, not clean-shaped, and must stay
    # fail-closed even though the nitpick section is present.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"Actionable comments: 3\\n🧹 Nitpick comments (1)\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "fail-closed" ]
}

@test "verdict: digit-free 'no actionable' prose does not block the shape-3 pass" {
    setup_jq
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"No actionable comments were generated.\\n🧹 Nitpick comments (2)\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "nitpick-clean" ]
}

@test "verdict: a body WITH the count header never takes the shape-3 path" {
    setup_jq
    # Furniture markers appear in every CR review body, findings included. The
    # header parse must win: 3 actionable + a nitpick section is FAILURE, not
    # nitpick-clean.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T1\",\"body\":\"**Actionable comments posted: 3**\\n🧹 Nitpick comments (1)\"}]" SUCCESS)"
    [ "$(verdict "$f")" = "findings:3" ]
}

@test "the shape-3 furniture guard is present in the action, not just here" {
    grep -qF "$NITPICK_CLEAN_RE" "$ACTION"
}

@test "verdict: a rate-limited SUCCESS is NOT review evidence" {
    setup_jq
    # CodeRabbit publishes state=SUCCESS with description "Review rate limited"
    # for a review that never ran (observed on this PR, head 51c74fe 03:28:01).
    # Reading only .state, this head — with no review node at all — would pass
    # as "completed with no review (skipped/clean)": an unreviewed green.
    f="$(payload '[]' SUCCESS 'Review rate limited')"
    [ "$(verdict "$f")" = "unsettled" ]
}

@test "verdict: a genuinely completed SUCCESS with no review node still passes" {
    setup_jq
    # The CR-skipped-review case (trivial/docs/workflow diffs) must keep working;
    # over-tightening here wedges every skipped PR, the #536/#1718 failure.
    f="$(payload '[]' SUCCESS 'Review completed')"
    [ "$(verdict "$f")" = "not-reviewed" ]
}

@test "verdict: rate-limited SUCCESS beside a blank reply is still not evidence" {
    setup_jq
    # The exact live shape: CR rate limited on the head AND a blank thread reply
    # present. Both defects at once — must not resolve to green.
    f="$(payload "[{$CR_NODE,\"submittedAt\":\"T2\",\"body\":\"\"}]" SUCCESS 'Review rate limited')"
    [ "$(verdict "$f")" = "unsettled" ]
}

@test "the rate-limit guard is present in the action, not just in this suite" {
    # verdict() restates the marker for readability; if the action stopped using
    # it these tests would keep passing against a gate that no longer checks.
    # -F: the stored pattern is an ERE; matching it as a basic-grep regex would
    # silently fail on the alternation and read as "guard missing".
    grep -qF "$RATE_LIMIT_RE" "$ACTION"
    # The guard is inert unless the GraphQL query actually selects description.
    grep -qF 'on StatusContext{ context state description }' "$ACTION"
}

@test "selftest: without the guard, a rate-limited SUCCESS would pass" {
    setup_jq
    # Proves the guard is load-bearing: same fixture, guard bypassed.
    f="$(payload '[]' SUCCESS 'Review rate limited')"
    state=$(jq -r -f "$BATS_TEST_TMPDIR/ctx.jq" "$f")
    desc=$(jq -r -f "$BATS_TEST_TMPDIR/desc.jq" "$f")
    [ "$state" = "SUCCESS" ]                                   # old code: -> pass
    printf '%s' "$desc" | grep -qiE "$RATE_LIMIT_RE"           # new code: -> unsettled
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

# ============================================================================
# Stale-evidence auto-nudge (shapes 1-2, 2026-08-12 entry) — the gate posts
# `@coderabbitai full review` itself when CR's status on the head is a stale
# rate-limit SUCCESS while a completed clean pass exists only as a comment.
# These tests run the REAL function, extracted from action.yml, against a
# stubbed `gh` — same no-drift contract as the jq extraction above.
# ============================================================================

# Extract maybe_nudge_full_review() from the action's run block. The block's
# base indent is 8 spaces, so the function's closing brace is exactly
# "        }" — inner constructs sit deeper and cannot end the match early.
setup_nudge() {
    awk '/maybe_nudge_full_review\(\) \{/{f=1} f{print} f && /^        \}$/{exit}' \
        "$ACTION" > "$BATS_TEST_TMPDIR/nudge.fn"
    # Non-vacuity: an extraction miss must fail loudly, not test nothing.
    grep -q 'cr-full-review-nudge' "$BATS_TEST_TMPDIR/nudge.fn"
    grep -q 'gh api'               "$BATS_TEST_TMPDIR/nudge.fn"
    POST_LOG="$BATS_TEST_TMPDIR/posts.log"; : > "$POST_LOG"
    TSV="$BATS_TEST_TMPDIR/comments.tsv";   : > "$TSV"
    export POST_LOG TSV
    SHA=0123456789abcdef0123456789abcdef01234567
    OWNER=o REPO=r PR=1 NUDGE_BUDGET=3
    export SHA OWNER REPO PR NUDGE_BUDGET
}

# Stub gh: the comments fetch serves the TSV fixture (rc from GH_FETCH_RC);
# any other invocation is a POST and is appended to the log, one arg per line.
gh() {
    case "$*" in
        *"comments?per_page="*) cat "$TSV"; return "${GH_FETCH_RC:-0}" ;;
        *) printf '%s\n' "$@" >> "$POST_LOG" ;;
    esac
}

# row <login> <updated_at> <body> — append one comment to the TSV fixture in
# the function's wire format: login TAB updated_at TAB base64(body).
# tr -d '\n' keeps the base64 single-line and portable (no -w0).
row() {
    printf '%s\t%s\t%s\n' "$1" "$2" "$(printf '%s' "$3" | base64 | tr -d '\n')" >> "$TSV"
}

run_nudge() {
    # shellcheck disable=SC1090
    source "$BATS_TEST_TMPDIR/nudge.fn"
    nudge_attempted=false
    # `run` shields bats' errexit: the function tolerates a failing gh fetch
    # by design (the real action runs under `set +e`), and must exit 0 there.
    run maybe_nudge_full_review
    [ "$status" -eq 0 ]
}

@test "nudge: posts once given a comments-only clean pass and no prior marker" {
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review complete — no actionable findings.'
    run_nudge
    grep -q '@coderabbitai full review'          "$POST_LOG"
    grep -q "cr-full-review-nudge:${SHA}"        "$POST_LOG"
}

@test "nudge: once per head - an existing marker for this SHA suppresses it" {
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review complete — no actionable findings.'
    row 'github-actions[bot]' '2026-08-13T12:01:00Z' "@coderabbitai full review <!-- cr-full-review-nudge:${SHA} -->"
    run_nudge
    [ ! -s "$POST_LOG" ]
}

@test "nudge: budget cap - three prior nudges on other heads suppress a fourth" {
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review complete — no actionable findings.'
    row 'github-actions[bot]' '2026-08-13T10:00:00Z' 'cr-full-review-nudge:aaaa'
    row 'github-actions[bot]' '2026-08-13T10:01:00Z' 'cr-full-review-nudge:bbbb'
    row 'github-actions[bot]' '2026-08-13T10:02:00Z' 'cr-full-review-nudge:cccc'
    run_nudge
    [ ! -s "$POST_LOG" ]
}

@test "nudge: the persistent walkthrough comment is not clean-pass evidence" {
    setup_nudge
    # CR edits the walkthrough in place; its "No actionable comments…" text
    # survives across heads. Trusting it would burn the once-per-head nudge
    # inside a rate-limit window, on a reply that just says "limit reached".
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' '<!-- walkthrough_start --> No actionable comments were generated in the recent review.'
    run_nudge
    [ ! -s "$POST_LOG" ]
}

@test "nudge: clean-pass prose from a non-CR author is not evidence" {
    setup_nudge
    row 'someuser' '2026-08-13T12:00:00Z' 'there are no actionable findings in my opinion'
    run_nudge
    [ ! -s "$POST_LOG" ]
}

@test "nudge: no clean-pass evidence at all -> no post" {
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review limit reached. Next review available in 115 minutes.'
    run_nudge
    [ ! -s "$POST_LOG" ]
}

@test "nudge: a failed comment fetch never nudges (dedup marker invisible)" {
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review complete — no actionable findings.'
    GH_FETCH_RC=1
    run_nudge
    unset GH_FETCH_RC
    [ ! -s "$POST_LOG" ]
}

@test "nudge: evaluated at most once per action run" {
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review complete — no actionable findings.'
    source "$BATS_TEST_TMPDIR/nudge.fn"
    nudge_attempted=false
    maybe_nudge_full_review
    maybe_nudge_full_review   # second call in the same run: marker not yet
                              # visible in the fixture — must not double-post
    [ "$(grep -c '@coderabbitai full review' "$POST_LOG")" -eq 1 ]
}

@test "workflow grants the comment-POST scope and NOT more (least privilege)" {
    # issues:write alone authorizes the nudge's PR-comment POST (PR comments go
    # through the issues endpoint); pull-requests stays read for the metadata /
    # file-list fetches. pull-requests:write would be unused token surface.
    grep -qE '^  issues: write$'         "$WF"
    grep -qE '^  pull-requests: read$'   "$WF"
    ! grep -qE '^  pull-requests: write$' "$WF"
}

@test "nudge: a forged marker from a non-bot commenter does not suppress recovery" {
    # Markers are authenticated by author (github-actions[bot], the login the
    # workflow token posts as). A drive-by commenter pasting the marker — or
    # three of them to drain the budget — must not stop the nudge.
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review complete — no actionable findings.'
    row 'someuser' '2026-08-13T12:01:00Z' "@coderabbitai full review <!-- cr-full-review-nudge:${SHA} -->"
    row 'someuser' '2026-08-13T12:02:00Z' 'cr-full-review-nudge:aaaa'
    row 'someuser' '2026-08-13T12:03:00Z' 'cr-full-review-nudge:bbbb'
    row 'someuser' '2026-08-13T12:04:00Z' 'cr-full-review-nudge:cccc'
    run_nudge
    grep -q '@coderabbitai full review' "$POST_LOG"
}

@test "the nudge call is wired into the rate-limit branch, not just defined" {
    # The function existing is not enough — it must run where the wedge occurs.
    awk "/grep -qiE 'rate.\?limit\|limit reached'/,/^            fi\$/" "$ACTION" \
        | grep -q 'maybe_nudge_full_review'
}

@test "nudge: recency - a busy signal newer than the clean reply suppresses it" {
    setup_nudge
    # The burn scenario: head H2 just bounced on the rate limit; the only
    # clean-pass reply is from earlier head H1. Nudging now would spend the
    # once-per-head marker on a "limit reached" reply.
    row 'coderabbitai[bot]' '2026-08-13T10:00:00Z' 'Review complete — no actionable findings.'
    row 'coderabbitai[bot]' '2026-08-13T11:00:00Z' '## Review limit reached — Next review available in: 115 minutes'
    run_nudge
    [ ! -s "$POST_LOG" ]
}

@test "nudge: recency - a clean reply newer than the busy signal posts" {
    setup_nudge
    # The genuine shape-1 wedge: the window reopened, a comment-only clean
    # pass completed AFTER the rate-limit notice, and the stale status sits.
    row 'coderabbitai[bot]' '2026-08-13T11:00:00Z' '## Review limit reached — Next review available in: 115 minutes'
    row 'coderabbitai[bot]' '2026-08-13T13:00:00Z' 'Review complete — no actionable findings.'
    run_nudge
    grep -q '@coderabbitai full review' "$POST_LOG"
}

@test "nudge: an in-progress walkthrough edit newer than the clean reply suppresses it" {
    setup_nudge
    row 'coderabbitai[bot]' '2026-08-13T12:00:00Z' 'Review complete — no actionable findings.'
    row 'coderabbitai[bot]' '2026-08-13T12:30:00Z' '<!-- walkthrough_start --> Currently processing new changes in this PR.'
    run_nudge
    [ ! -s "$POST_LOG" ]
}

@test "nudge: a quoted rate-limit LEARNING inside the clean reply is not a busy signal" {
    setup_nudge
    # CR command replies quote stored repo learnings verbatim, and ours contain
    # "Review rate limited". If the busy regex matched that phrase, the genuine
    # clean reply would suppress ITSELF — the feature dead in its target
    # scenario. The busy match is on notice wording, not on 'rate limit'.
    row 'coderabbitai[bot]' '2026-08-13T13:00:00Z' 'Review complete — no actionable findings. Learnings used: a status with description Review rate limited is not review evidence.'
    run_nudge
    grep -q '@coderabbitai full review' "$POST_LOG"
}

@test "the nudge is also wired into the not-settled arm (shape 2, no status at all)" {
    # A comment-only clean pass can complete with NO CodeRabbit StatusContext
    # on the head (cr_ctx ABSENT) — the wedge parks in the `*)` arm, one door
    # over from the rate-limit branch. The recency guard is what keeps this
    # call quiet while CR is genuinely still working.
    grep -qE '^\s*\*\)\s*maybe_nudge_full_review; return 1 ;;' "$ACTION"
}

@test "the drifted-header digit guard is present in the action, not just here" {
    # verdict() restates the negative half of the shape-3 discrimination; this
    # pins the action's copy so removing it there cannot pass silently.
    grep -qF "grep -i 'actionable' | grep -q '[0-9]'" "$ACTION"
}
