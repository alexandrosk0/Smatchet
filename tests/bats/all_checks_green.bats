#!/usr/bin/env bats
# tests/bats/all_checks_green.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/all-checks-green.sh — the decision logic of
# the `All checks green (block-on-any-red)` aggregate check
# (.github/workflows/all-checks-green.yml; backlog infra/2026-10-04-native-auto-
# merge-merges-past-a-red-non-required-check, postmortems.md 2026-10-04 #2286).
#
# Fixture: tests/fixtures/all_checks_green_pr2286.json — PR #2286's real head
# check list (52 runs + 2 statuses, final states). `replay <ts>` rebuilds the head
# as GitHub showed it at <ts>: runs not yet started are dropped, runs completed
# later read in_progress. The three replay timestamps are the postmortem's.
#
# Live-mode cases drive the real poll loop against a stub `gh` (canned REST pages
# per call) with a no-op sleep (ACG_SLEEP_BIN=true).
#
# Requires: bash, bats, jq.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    ACG="$REPO_ROOT/agents/scripts/core/all-checks-green.sh"
    FIXTURE="$REPO_ROOT/tests/fixtures/all_checks_green_pr2286.json"
    SNAP="$BATS_TEST_TMPDIR/snap.json"
    SELF="All checks green (block-on-any-red)"
    export REPO_ROOT ACG FIXTURE SNAP SELF
    unset ACG_REQUIRED_CONTEXTS ACG_SELF
}

# replay <iso-ts|final> [jq-edit] — the #2286 head as of <ts> (or its final
# state), then an optional jq edit, written to $SNAP.
replay() {
    local ts="$1" edit="${2:-.}"
    if [ "$ts" = "final" ]; then ts="9999-12-31T23:59:59Z"; fi
    jq --arg t "$ts" "
      def setrun(\$n; \$c): .check_runs |= map(if .name == \$n then .status = \"completed\" | .conclusion = \$c else . end);
      def setstatus(\$n; \$s): .statuses |= map(if .context == \$n then .state = \$s else . end);
      .check_runs |= [ .[] | select((.started_at // \"\") <= \$t)
                       | if ((.completed_at // \"\") > \$t)
                         then .status = \"in_progress\" | .conclusion = null | .completed_at = null
                         else . end ]
      | .statuses |= [ .[] | select((.created_at // \"\") <= \$t) ]
      | $edit" "$FIXTURE" > "$SNAP"
}

# The #2286 head with Bucket-E fixed and CodeRabbit reviewed — the green baseline.
GREEN_EDIT='setrun("Bucket-E UI tests (Mesa headless GL)"; "success") | setstatus("CR findings (0 actionable)"; "success")'

# ---------- the #2286 replay (the backlog entry's acceptance cases) ----------

@test "#2286 @ 01:43:54Z (auto-merge armed): six checks still running -> pending" {
    replay "2026-10-04T01:43:54Z"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 3 ]
    [[ "$output" == *"verdict=pending"* ]]
    for c in "Sanitizer (ASAN via MSVC)" "Sanitizer (UBSan via Clang)" "Bucket-E UI tests (Mesa headless GL)" \
             "Bucket-E Jira fixture-backend (Mesa GL, hard)" "CodeQL analyze (c-cpp)"; do
        [[ "$output" == *"PENDING     $c (in_progress)"* ]]
    done
    # The advisory texture-guard lane is running too, but never holds the verdict.
    [[ "$output" == *"ADVISORY    Mobile texture-guard smoke (Mesa headless GL, advisory) (in_progress)"* ]]
}

@test "#2286 @ 01:56:45Z: a red Bucket-E fails fast while other checks still run" {
    replay "2026-10-04T01:56:45Z"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    [[ "$output" == *"verdict=failure"* ]]
    [[ "$output" == *"RED         Bucket-E UI tests (Mesa headless GL) (failure)"* ]]
    [[ "$output" == *"PENDING     Sanitizer (UBSan via Clang) (in_progress)"* ]]
}

@test "#2286 @ 01:59:29Z (last required check green -  GitHub merged): aggregate still red" {
    replay "2026-10-04T01:59:29Z"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    [[ "$output" == *"RED         Bucket-E UI tests (Mesa headless GL) (failure)"* ]]
}

@test "#2286 @ 01:59:29Z with Bucket-E green: CodeQL analyze in progress -> still pending" {
    replay "2026-10-04T01:59:29Z" "$GREEN_EDIT"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 3 ]
    [[ "$output" == *"PENDING     CodeQL analyze (c-cpp) (in_progress)"* ]]
    [ "$(grep -c '^  PENDING' <<<"$output")" -eq 1 ]
}

@test "#2286 final with only the advisory texture-guard non-green -> success" {
    replay final "$GREEN_EDIT | setrun(\"Mobile texture-guard smoke (Mesa headless GL, advisory)\"; \"failure\")"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict=success"* ]]
    [[ "$output" == *"ADVISORY    Mobile texture-guard smoke (Mesa headless GL, advisory) (failure)"* ]]
    # Intent section has three red runs in OLDER suites + a green newest one: no block.
    [[ "$output" != *"Intent section"* ]]
}

# ---------- rule coverage ----------

@test "latest run per name: a red in the NEWEST suite blocks despite older greens" {
    replay final "$GREEN_EDIT | .check_runs |= map(if .name == \"Intent section\" and .check_suite.id == 100675780502 then .conclusion = \"failure\" else . end)"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    [[ "$output" == *"RED         Intent section (failure)"* ]]
}

@test "a queued re-run attempt (same suite, newer run id, no started_at) supersedes the red attempt" {
    replay "2026-10-04T01:59:29Z" ".check_runs += [{name: \"Bucket-E UI tests (Mesa headless GL)\", status: \"queued\",
        conclusion: null, started_at: null, id: 999999999999, check_suite: {id: 100675454637}}]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 3 ]
    [[ "$output" == *"PENDING     Bucket-E UI tests (Mesa headless GL) (queued)"* ]]
    [[ "$output" != *"RED "* ]]
}

@test "cancelled / timed_out / action_required / startup_failure count as red" {
    for c in cancelled timed_out action_required startup_failure; do
        replay final "$GREEN_EDIT | setrun(\"TSan Linux subset (Clang)\"; \"$c\")"
        run bash "$ACG" --fixture "$SNAP"
        [ "$status" -eq 1 ]
        [[ "$output" == *"RED         TSan Linux subset (Clang) ($c)"* ]]
    done
}

@test "neutral / skipped conclusions and a success status pass" {
    replay final "$GREEN_EDIT | setrun(\"TSan Linux subset (Clang)\"; \"neutral\") | setrun(\"Pillar 2 scanner\"; \"skipped\")"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
}

@test "a commit status in error is red" {
    replay final "$GREEN_EDIT | setstatus(\"CodeRabbit\"; \"error\")"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    [[ "$output" == *"RED         CodeRabbit (error)"* ]]
}

@test "self-exclusion: this check's own running / cancelled runs never count" {
    replay final "$GREEN_EDIT | .check_runs += [
        {name: \"$SELF\", status: \"in_progress\", conclusion: null, id: 1, check_suite: {id: 999999999999}},
        {name: \"$SELF\", status: \"completed\", conclusion: \"cancelled\", id: 2, check_suite: {id: 1}}]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
    [[ "$output" != *"$SELF ("* ]]
}

@test "a required context that has not reported yet holds the verdict pending" {
    replay final "$GREEN_EDIT | .check_runs |= map(select(.name != \"Windows + MSVC\"))"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 3 ]
    [[ "$output" == *"NOT-YET     Windows + MSVC (required context, not reported)"* ]]
}

@test "the advisory exemption does not cover a REQUIRED context" {
    replay final "$GREEN_EDIT | setrun(\"Mobile texture-guard smoke (Mesa headless GL, advisory)\"; \"failure\")"
    ACG_REQUIRED_CONTEXTS="Mobile texture-guard smoke (Mesa headless GL, advisory)" run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    [[ "$output" == *"RED         Mobile texture-guard smoke (Mesa headless GL, advisory) (failure)"* ]]
}

@test "plan-lock-out-of-band ALONE does not downgrade a red Plan-lock gate (poller parity)" {
    replay final "$GREEN_EDIT | setrun(\"Plan-lock gate\"; \"failure\")"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    # The merge-gates poller requires a plan-lock-disposition trail too; the
    # aggregate must not be laxer than the poller it stands in for.
    replay final "$GREEN_EDIT | setrun(\"Plan-lock gate\"; \"failure\") | .labels = [\"plan-lock-out-of-band\"]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    [[ "$output" == *"RED         Plan-lock gate (failure)"* ]]
    # A disposition without the out-of-band label is not an override either.
    replay final "$GREEN_EDIT | setrun(\"Plan-lock gate\"; \"failure\") | .labels = [\"plan-lock-disposition:sibling-merged\"]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
}

@test "plan-lock-out-of-band + a plan-lock-disposition (label or body) downgrades only the Plan-lock gate" {
    replay final "$GREEN_EDIT | setrun(\"Plan-lock gate\"; \"failure\") | .labels = [\"plan-lock-out-of-band\", \"plan-lock-disposition:sibling-merged\"]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
    [[ "$output" == *"DOWNGRADED  Plan-lock gate (failure)"* ]]
    replay final "$GREEN_EDIT | setrun(\"Plan-lock gate\"; \"failure\") | .labels = [{name: \"plan-lock-out-of-band\"}] | .body = \"- plan-lock-disposition: overlap is docs-only\""
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
    # An empty body marker is not a disposition.
    replay final "$GREEN_EDIT | setrun(\"Plan-lock gate\"; \"failure\") | .labels = [\"plan-lock-out-of-band\"] | .body = \"plan-lock-disposition:   \""
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
    replay final "$GREEN_EDIT | setrun(\"Pillar 2 scanner\"; \"failure\") | .labels = [\"plan-lock-out-of-band\", \"plan-lock-disposition:x\"]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 1 ]
}

@test "tests- / perf- / intent-out-of-band downgrade their named checks" {
    replay final "$GREEN_EDIT | setrun(\"Test-delta gate\"; \"failure\") | setrun(\"Perf PR-fast (windows-2022)\"; \"failure\")
                  | .check_runs |= map(if .name == \"Intent section\" then .conclusion = \"failure\" else . end)
                  | .labels = [{name: \"tests-out-of-band\"}, {name: \"perf-out-of-band\"}, {name: \"intent-out-of-band\"}]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
    [ "$(grep -c '^  DOWNGRADED' <<<"$output")" -eq 3 ]
}

@test "cr-out-of-band needs a cr-disposition to release a pending CR findings status" {
    replay final "setrun(\"Bucket-E UI tests (Mesa headless GL)\"; \"success\") | .labels = [\"cr-out-of-band\"]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 3 ]
    [[ "$output" == *"PENDING     CR findings (0 actionable) (pending)"* ]]
    replay final "setrun(\"Bucket-E UI tests (Mesa headless GL)\"; \"success\") | .labels = [\"cr-out-of-band\", \"cr-disposition:rate-limit-acked\"]"
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
    replay final "setrun(\"Bucket-E UI tests (Mesa headless GL)\"; \"success\") | .labels = [\"cr-out-of-band\"] | .body = \"cr-disposition: rate-limited for hours\""
    run bash "$ACG" --fixture "$SNAP"
    [ "$status" -eq 0 ]
}

@test "workflow wiring: job name == ACG_SELF == script default; always reports" {
    WF="$REPO_ROOT/.github/workflows/all-checks-green.yml"
    [ "$(grep -cxF "    name: $SELF" "$WF")" -eq 1 ]
    [ "$(grep -cxF "          ACG_SELF: $SELF" "$WF")" -eq 1 ]
    grep -qF "ACG_SELF:-$SELF}" "$ACG"
    # Always-report: no paths filter, no job-level `if:` (a skipped run reads as success).
    run grep -nE '^[[:space:]]*(paths|paths-ignore):|^    if:' "$WF"
    [ "$status" -eq 1 ]
    grep -qE '^    timeout-minutes: [0-9]+$' "$WF"
}

@test "usage errors exit 2" {
    run bash "$ACG" --fixture "$BATS_TEST_TMPDIR/absent.json"
    [ "$status" -eq 2 ]
    run bash "$ACG" --bogus
    [ "$status" -eq 2 ]
    run env -u ACG_PR ACG_REPO=o/r ACG_SHA=0000000000000000000000000000000000000000 bash "$ACG"
    [ "$status" -eq 2 ]
}

# ---------- live mode (poll loop) against a stub gh ----------

# stub_gh — a `gh` on PATH that serves canned REST responses: per kind
# (runs | status | pr) the Nth call reads $STUB/<kind>.<N>.json, falling back to
# $STUB/<kind>.json; a $STUB/<kind>.<N>.fail file makes that call fail. --jq is
# applied with the real jq, so the script's own field projections are exercised.
stub_gh() {
    STUB="$BATS_TEST_TMPDIR/stub"
    mkdir -p "$STUB/bin"
    cat > "$STUB/bin/gh" <<'GH'
#!/usr/bin/env bash
endpoint=""; filter="."
while [ $# -gt 0 ]; do
    case "$1" in
        api|--paginate) ;;
        --jq) filter="$2"; shift ;;
        *) endpoint="$1" ;;
    esac
    shift
done
case "$endpoint" in
    */check-runs*) kind=runs ;;
    */status*) kind=status ;;
    */pulls/*) kind=pr ;;
    *) echo "stub gh: unexpected endpoint $endpoint" >&2; exit 1 ;;
esac
n=$(( $(cat "$STUB/$kind.count" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$STUB/$kind.count"
[ -e "$STUB/$kind.$n.fail" ] && { echo "stub gh: HTTP 502" >&2; exit 1; }
f="$STUB/$kind.$n.json"; [ -f "$f" ] || f="$STUB/$kind.json"
jq -c "$filter" "$f"
GH
    chmod +x "$STUB/bin/gh"
    export STUB
}

# serve <kind-file-stem> — REST-shape the current $SNAP into $STUB/<stem>.json
# (stem: runs | runs.<N> | status | status.<N>).
serve() {
    case "$1" in
        runs*) jq '{total_count: (.check_runs | length), check_runs}' "$SNAP" > "$STUB/$1.json" ;;
        status*) jq '{state: "success", statuses}' "$SNAP" > "$STUB/$1.json" ;;
    esac
}

serve_pr() { # <head-sha> [state]
    jq -n --arg h "$1" --arg s "${2:-open}" '{head: {sha: $h}, state: $s, labels: [], body: ""}' > "$STUB/pr.json"
}

HEAD_SHA="dfa2e0ce6c52711d0825e5aa772818785050887f"

run_live() {
    run env PATH="$STUB/bin:$PATH" ACG_SLEEP_BIN=true ACG_REPO=o/r ACG_PR=2286 ACG_SHA="$HEAD_SHA" \
        ACG_REQUIRED_CONTEXTS="$(jq -r '.required_contexts | join("\n")' "$FIXTURE")" "$@" \
        bash "$ACG"
}

@test "live: the first blocking red fails fast (one poll, ::error names it)" {
    stub_gh; replay "2026-10-04T01:56:45Z"; serve runs; serve status; serve_pr "$HEAD_SHA"
    run_live
    [ "$status" -eq 1 ]
    [ "$(cat "$STUB/runs.count")" -eq 1 ]
    [[ "$output" == *"::error title=All checks green — blocking red::Bucket-E UI tests (Mesa headless GL) (failure)"* ]]
}

@test "live: all green passes only after a settle re-poll sees the same set" {
    stub_gh; replay final "$GREEN_EDIT"; serve runs; serve status; serve_pr "$HEAD_SHA"
    run_live
    [ "$status" -eq 0 ]
    [ "$(cat "$STUB/runs.count")" -eq 2 ]
    [[ "$output" == *"PASS — every other check is terminal and green"* ]]
}

@test "live: a check appearing during the settle window is waited for" {
    stub_gh; serve_pr "$HEAD_SHA"
    replay final "$GREEN_EDIT"; serve runs.1; serve status
    replay final "$GREEN_EDIT | .check_runs += [{name: \"Late lane\", status: \"queued\", conclusion: null, id: 5, check_suite: {id: 100700000000}}]"
    serve runs.2
    replay final "$GREEN_EDIT | .check_runs += [{name: \"Late lane\", status: \"completed\", conclusion: \"success\", id: 5, check_suite: {id: 100700000000}}]"
    serve runs
    run_live
    [ "$status" -eq 0 ]
    [ "$(cat "$STUB/runs.count")" -eq 4 ]
    [[ "$output" == *"waiting on: Late lane (queued)"* ]]
}

@test "live: a moved PR head voids the run (exit 1, superseded)" {
    stub_gh; replay final "$GREEN_EDIT"; serve runs; serve status
    serve_pr "1111111111111111111111111111111111111111"
    run_live
    [ "$status" -eq 1 ]
    [[ "$output" == *"superseded"* ]]
}

@test "live: a PR that is no longer open voids the run" {
    stub_gh; replay final "$GREEN_EDIT"; serve runs; serve status; serve_pr "$HEAD_SHA" closed
    run_live
    [ "$status" -eq 1 ]
    [[ "$output" == *"is closed"* ]]
}

@test "live: still pending when the budget is spent -> timeout is red" {
    stub_gh; replay "2026-10-04T01:43:54Z"; serve runs; serve status; serve_pr "$HEAD_SHA"
    run_live ACG_MAX_WAIT_SECONDS=0
    [ "$status" -eq 1 ]
    [[ "$output" == *"timed out"* ]]
    [[ "$output" == *"PENDING     CodeQL analyze (c-cpp) (in_progress)"* ]]
}

@test "live: transient API errors are retried; persistent ones fail closed (exit 2)" {
    stub_gh; replay final "$GREEN_EDIT"; serve runs; serve status; serve_pr "$HEAD_SHA"
    : > "$STUB/runs.1.fail"
    run_live
    [ "$status" -eq 0 ]
    [[ "$output" == *"GitHub API error (1/10)"* ]]
    rm -f "$STUB"/*.count
    for i in 1 2 3; do : > "$STUB/runs.$i.fail"; done
    run_live ACG_MAX_API_FAILURES=3
    [ "$status" -eq 2 ]
    [[ "$output" == *"GitHub API unavailable"* ]]
}
