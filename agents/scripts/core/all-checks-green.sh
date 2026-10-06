#!/usr/bin/env bash
# all-checks-green.sh — decision logic of the `All checks green (block-on-any-red)`
# aggregate check (.github/workflows/all-checks-green.yml).
# ----------------------------------------------------------------------------
# WHY (postmortems.md 2026-10-04 PR #2286; backlog infra/2026-10-04-native-auto-
# merge-merges-past-a-red-non-required-check): GitHub's native auto-merge waits
# only on the branch-protection REQUIRED contexts. Block-on-any-red lives in the
# merge-gates poller, which only runs when the merge goes through safe-merge.sh /
# git-janitor / the merge-watcher — so every other arm path (a harness auto-merge
# tool, a GitHub-MCP tool, the web UI, bare `gh pr merge --auto`) merged past a
# red or still-pending NON-required check. This script turns block-on-any-red into
# ONE check GitHub itself can require: it polls the PR head's check runs + commit
# statuses until every OTHER check is terminal, fails fast on the first blocking
# red, and passes only when everything is terminal and green.
#
# RULES — the CI half of merge-gates.d/10-gate-filter.sh, ported to the REST
# checks API (the GraphQL GATE_FILTER also scores CodeRabbit / Bugbot / comments /
# review state, which are not checks, so it cannot be reused verbatim):
#   * latest run per name — newest check suite (suite id, monotonic in creation
#     order: the REST stand-in for the filter's checkSuite.createdAt), then the
#     newest run in it (run id, also creation order — unlike started_at it is set
#     on a still-queued re-run); a re-run supersedes, a cancelled concurrency
#     twin in an older suite does not count. Runs are fetched with filter=all so
#     a queued / in-progress re-run attempt is never hidden behind the completed
#     attempt it replaces;
#   * red = a completed run concluding failure / timed_out / cancelled /
#     action_required / startup_failure, or a status in failure / error;
#     pending = a run not yet completed, or a status in pending / expected;
#   * advisory-name exemption — a check whose NAME contains "advisory" (any case)
#     never blocks, unless it is itself a required context;
#   * self-exclusion — every run / status named exactly like this check;
#   * required-absent — a branch_protection.required_contexts name with no run or
#     status on the head yet counts as pending (it has not reported);
#   * the poller's out-of-band downgrades: tests-out-of-band (Test-delta gate),
#     perf-out-of-band (Perf PR-fast*), intent-out-of-band (Intent section),
#     plan-lock-out-of-band PAIRED with a plan-lock-disposition (label
#     `plan-lock-disposition:<why>` or a PR-body marker) for the Plan-lock gate,
#     and cr-out-of-band PAIRED with a cr-disposition (same two forms) for the
#     CR finding check + `CR findings*` status — red OR pending. The poller's
#     stale-override freshness conjunct is NOT ported: the two label-reactive
#     checks it guards are themselves required contexts, so GitHub still waits on
#     their post-label re-run directly, and the settle re-poll below sees a re-run
#     that lands after the label.
#
# LIVE MODE (default — the workflow): env ACG_REPO (owner/name), ACG_PR, ACG_SHA
# (the PR head the run is attached to); GH_TOKEN for `gh api`. Polls every
# ACG_POLL_SECONDS (default 90, floor 30). REST BUDGET — each poll reads 2-3
# lists from the 1,000/h per-repo GITHUB_TOKEN budget every workflow shares, so:
#   * every GET is conditional (If-None-Match with the ETag of the last 200 for
#     that page); an unchanged list answers 304, which GitHub does not count
#     against the rate limit, and the cached body is reused;
#   * X-RateLimit-Remaining is read off every response: below ACG_RL_LOW
#     (default 200) the poll interval widens x4; below ACG_RL_STOP (default 50),
#     or on a rate-limited 403/429, polling pauses until X-RateLimit-Reset (the
#     check stays pending meanwhile; the wait budget still bounds it).
# Failure modes:
#   * first blocking red -> exit 1 at once (fail fast);
#   * all terminal + green -> re-poll after ACG_SETTLE_SECONDS (default 60) and
#     pass only if the check set is unchanged (late checks: a dependent job, the
#     code-scanning result check, a label-triggered re-run);
#   * PR head moved -> exit 1 (this run's verdict is void; the new head gets
#     its own run);
#   * PR no longer open (merged / closed) -> exit 0 with a "verdict moot"
#     notice: a red here would leave a FAILURE check on nearly every merged
#     head (a merge through safe-merge.sh does not wait for this check), which
#     reads as a gate escape to anything auditing merged heads;
#   * still pending at ACG_MAX_WAIT_SECONDS (default 10200 = 170 min) -> exit 1
#     (a timeout is red, fail-closed; re-running the job or any label change
#     clears it). The budget must outlast the slowest LEGITIMATE path, not the
#     typical one: CodeQL analyze alone may run its 90-min timeout-minutes, and
#     Windows + MSVC (45) feeds Bucket-E (45) serially, each after queueing for
#     a runner — 170 min covers either with queue slack. The workflow's
#     timeout-minutes sits above budget + the longest post-deadline sleep, so
#     the script, not the runner, reports the pending set (bats pins it);
#   * ACG_MAX_API_FAILURES (default 10) consecutive API failures -> exit 2.
# The PR (head, state, labels, body) is re-read every ACG_PR_REFRESH_POLLS
# (default 5) polls and always before a terminal verdict.
#
# FIXTURE MODE: --fixture <json> evaluates one snapshot and exits — bats replays
# the #2286 check list through it. The JSON carries `check_runs` / `statuses`
# (REST shapes), optional `labels`, `body`, `required_contexts`, `self`.
#
# Env (both modes): ACG_SELF (this check's name), ACG_REQUIRED_CONTEXTS
# (NEWLINE-separated — check names can contain commas; overrides the fixture's /
# project.config.json's set — set but empty disables the required-absent rule).
# Test seams: ACG_SLEEP_BIN (default `sleep`), ACG_PER_PAGE (default 100).
#
# Exit: 0 success / verdict moot (PR no longer open) · 1 failure (red, timeout,
#       superseded) · 2 usage / API error · 3 pending (fixture mode only).
# Tests: tests/bats/all_checks_green.bats.
# ----------------------------------------------------------------------------
set -uo pipefail

SELF_NAME="${ACG_SELF:-All checks green (block-on-any-red)}"
# Same config resolution as merge-gates.sh (PC_CONFIG_FILE, else the repo root's).
CONFIG_FILE="${PC_CONFIG_FILE:-$(dirname "$0")/../../../project.config.json}"

die() { echo "all-checks-green: $*" >&2; exit 2; }

command -v jq >/dev/null 2>&1 || die "jq not on PATH"

# The verdict program. Input: {check_runs, statuses, labels, body}; $self: this
# check's name; $req: required-context names. Output: {verdict, failing, pending,
# absent, downgraded, exempt, total, fingerprint}.
# shellcheck disable=SC2016  # single-quoted jq program — $-refs are jq variables
ACG_FILTER='
# disposition — the same reader as merge-gates.d/10-gate-filter.sh: a
# `<prefix>:` label, or a `<prefix>:<reason>` PR-body marker.
def disposition($labels; $body; $prefix):
  ($labels | any(startswith($prefix + ":")))
  or (($body // "") | test($prefix + ":[[:space:]]*[^[:space:]]"; "i"));
([.labels[]? | if type == "object" then (.name // "") else . end]) as $labels
| ($labels | any(. == "tests-out-of-band")) as $tests
| ($labels | any(. == "perf-out-of-band")) as $perf
| ($labels | any(. == "intent-out-of-band")) as $intent
| ($labels | any(. == "plan-lock-out-of-band")) as $planlock
| ($labels | any(. == "cr-out-of-band")) as $cr
| disposition($labels; .body; "cr-disposition") as $crdisp
| disposition($labels; .body; "plan-lock-disposition") as $planlockdisp
| ([$req[] | select(. != $self)]) as $reqNames
| ([.check_runs[]? | select((.name // "") != $self)]
   | group_by(.name // "")
   | map(sort_by([(.check_suite.id // 0), (.id // 0)]) | .[-1])
   | map({kind: "check", name: (.name // ""), id: (.id // 0),
          state: (if (.status // "") != "completed" then "pending"
                  elif ((.conclusion // "") | IN("failure", "timed_out", "cancelled", "action_required", "startup_failure")) then "fail"
                  else "pass" end),
          detail: (if (.status // "") != "completed" then (.status // "queued") else (.conclusion // "none") end)})) as $runs
| ([.statuses[]? | select((.context // "") != $self)]
   | group_by(.context // "")
   | map(sort_by([(.updated_at // .created_at // ""), (.id // 0)]) | .[-1])
   | map({kind: "status", name: (.context // ""), id: (.id // 0),
          state: (if ((.state // "") | IN("failure", "error")) then "fail"
                  elif ((.state // "") | IN("pending", "expected")) then "pending"
                  else "pass" end),
          detail: (.state // "none")})) as $stats
| ($runs + $stats) as $all
| ([$all[].name]) as $seen
| ([$reqNames[] | . as $n | select(($seen | any(. == $n)) | not)]) as $absent
| def exempt: (.name | ascii_downcase | contains("advisory"))
              and ((.name as $n | $reqNames | any(. == $n)) | not);
  def crwaived: $cr and $crdisp
                and ((.kind == "status" and (.name | test("^CR findings"; "i")))
                     or (.kind == "check" and (.name | test("^CR finding"; "i"))));
  def downgraded: ($tests and .kind == "check" and .name == "Test-delta gate")
                  or ($perf and .kind == "check" and (.name | startswith("Perf PR-fast")))
                  or ($intent and .kind == "check" and .name == "Intent section")
                  or ($planlock and $planlockdisp and .kind == "check" and .name == "Plan-lock gate")
                  or crwaived;
  def show: "\(.name) (\(.detail))";
  ([$all[] | select(.state == "fail" and (exempt | not) and (downgraded | not))]) as $failing
| ([$all[] | select(.state == "pending" and (exempt | not) and (crwaived | not))]) as $pending
| {verdict: (if ($failing | length) > 0 then "failure"
             elif (($pending | length) + ($absent | length)) > 0 then "pending"
             else "success" end),
   failing: [$failing[] | show],
   pending: [$pending[] | show],
   absent: $absent,
   downgraded: [$all[] | select(.state == "fail" and (exempt | not) and downgraded) | show],
   exempt: [$all[] | select(.state != "pass" and exempt) | show],
   total: ($all | length),
   fingerprint: ([$all[] | "\(.kind)|\(.name)|\(.id)|\(.detail)"] | sort | join("\n"))}
'

# required_contexts_json <fixture-or-empty> — JSON array of required-context names.
# Precedence: ACG_REQUIRED_CONTEXTS (set, even empty) > the fixture's
# `required_contexts` > project.config.json § branch_protection.required_contexts
# (live mode only — a fixture stays hermetic). Read with jq (UTF-8-safe for the
# em-dash context names, mirroring merge-gates.sh).
required_contexts_json() {
    local fixture="$1"
    if [ -n "${ACG_REQUIRED_CONTEXTS+x}" ]; then
        printf '%s\n' "$ACG_REQUIRED_CONTEXTS" | jq -R . | jq -sc 'map(select(length > 0))'
    elif [ -n "$fixture" ]; then
        jq -c '[.required_contexts[]?]' "$fixture"
    elif [ -f "$CONFIG_FILE" ]; then
        jq -c '[.branch_protection.required_contexts[]?]' "$CONFIG_FILE"
    else
        echo "all-checks-green: WARN no project.config.json — required-absent rule inert" >&2
        echo '[]'
    fi
}

# evaluate <doc.json> <req-json> — print the verdict object.
evaluate() {
    jq -c --arg self "$SELF_NAME" --argjson req "$2" "$ACG_FILTER" "$1"
}

# report <verdict-json> — human-readable breakdown (one line per non-green check).
report() {
    jq -r '"all-checks-green: verdict=\(.verdict) — \(.total) other check(s) on the head",
           (.failing[] | "  RED         \(.)"),
           (.pending[] | "  PENDING     \(.)"),
           (.absent[] | "  NOT-YET     \(.) (required context, not reported)"),
           (.downgraded[] | "  DOWNGRADED  \(.) (out-of-band label)"),
           (.exempt[] | "  ADVISORY    \(.) (advisory-named, never blocks)")' <<<"$1"
}

# ---------------------------------------------------------------- fixture mode
FIXTURE=""
case "${1:-}" in
    --fixture)
        [ $# -ge 2 ] || die "--fixture requires a path"
        FIXTURE="$2"
        ;;
    --fixture=*)
        FIXTURE="${1#--fixture=}"
        ;;
    -h|--help)
        awk 'NR > 1 && /^set -uo pipefail/ { exit } NR > 1' "$0"
        exit 0
        ;;
    "") ;;
    *) die "unknown argument: $1 (see --help)" ;;
esac

if [ -n "$FIXTURE" ]; then
    [ -f "$FIXTURE" ] || die "fixture not found: $FIXTURE"
    if [ -z "${ACG_SELF+x}" ]; then
        fixture_self="$(jq -r '.self // empty' "$FIXTURE")" || die "fixture is not valid JSON: $FIXTURE"
        [ -n "$fixture_self" ] && SELF_NAME="$fixture_self"
    fi
    req="$(required_contexts_json "$FIXTURE")" || die "cannot read required contexts"
    v="$(evaluate "$FIXTURE" "$req")" || die "cannot evaluate fixture: $FIXTURE"
    report "$v"
    case "$(jq -r .verdict <<<"$v")" in
        success) exit 0 ;;
        failure) exit 1 ;;
        *) exit 3 ;;
    esac
fi

# ------------------------------------------------------------------- live mode
command -v gh >/dev/null 2>&1 || die "gh not on PATH"
REPO="${ACG_REPO:-${GITHUB_REPOSITORY:-}}"
PR="${ACG_PR:-}"
SHA="${ACG_SHA:-}"
if [ -z "$REPO" ] || [ -z "$PR" ] || [ -z "$SHA" ]; then
    die "live mode needs ACG_REPO, ACG_PR and ACG_SHA"
fi
[[ "$PR" =~ ^[0-9]+$ ]] || die "ACG_PR must be a PR number (got '$PR')"
[[ "$SHA" =~ ^[0-9a-f]{40}$ ]] || die "ACG_SHA must be a 40-hex commit (got '$SHA')"

int_or() { if [[ "${1:-}" =~ ^[0-9]+$ ]]; then echo "$1"; else echo "$2"; fi; }
POLL="$(int_or "${ACG_POLL_SECONDS:-}" 90)"
[ "$POLL" -ge 30 ] || POLL=30
SETTLE="$(int_or "${ACG_SETTLE_SECONDS:-}" 60)"
MAX_WAIT="$(int_or "${ACG_MAX_WAIT_SECONDS:-}" 10200)"
MAX_API_FAIL="$(int_or "${ACG_MAX_API_FAILURES:-}" 10)"
PR_REFRESH="$(int_or "${ACG_PR_REFRESH_POLLS:-}" 5)"
[ "$PR_REFRESH" -ge 1 ] || PR_REFRESH=1
RL_LOW="$(int_or "${ACG_RL_LOW:-}" 200)"
RL_STOP="$(int_or "${ACG_RL_STOP:-}" 50)"
PER_PAGE="$(int_or "${ACG_PER_PAGE:-}" 100)"
[ "$PER_PAGE" -ge 1 ] && [ "$PER_PAGE" -le 100 ] || PER_PAGE=100
SLEEP_BIN="${ACG_SLEEP_BIN:-sleep}"

REQ="$(required_contexts_json "")" || die "cannot read required contexts"
WORK="$(mktemp -d)" || die "mktemp failed"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/cache" || die "mktemp failed"
echo '{"head":"","state":"open","labels":[],"body":""}' > "$WORK/pr.json"
RL_REMAINING=""   # X-RateLimit-Remaining of the last response ("" = not seen)
RL_RESET=""       # X-RateLimit-Reset (epoch seconds) of the last response

# header <name> — a response header's value from $WORK/hdr (case-insensitive).
header() {
    awk -v k="$1" 'BEGIN { k = tolower(k) }
        { i = index($0, ":"); if (i > 0 && tolower(substr($0, 1, i - 1)) == k) {
              v = substr($0, i + 1); sub(/^[ \t]+/, "", v); print v; exit } }' "$WORK/hdr"
}

# rest_get <key> <endpoint> — GET one REST page into $WORK/cache/<key>.body.
# Conditional: sends If-None-Match with the ETag of the last 200 for <key>; a
# 304 (exit non-zero from gh, headers still printed) reuses the cached body and
# costs no rate limit. Records X-RateLimit-Remaining / -Reset.
# Returns 0 ok · 1 error · 3 rate-limited (403/429 with no calls remaining).
rest_get() {
    local key="$1" endpoint="$2" code remaining reset etag=""
    local -a cond=()
    if [ -s "$WORK/cache/$key.etag" ] && [ -f "$WORK/cache/$key.body" ]; then
        etag="$(cat "$WORK/cache/$key.etag")"
        cond=(-H "If-None-Match: $etag")
    fi
    # Exit status deliberately ignored: gh exits non-zero on the 304 this
    # relies on — the status line below is the verdict.
    gh api -i ${cond[@]+"${cond[@]}"} "$endpoint" > "$WORK/raw" 2>"$WORK/raw.err" || :
    awk -v H="$WORK/hdr" -v B="$WORK/body" '
        BEGIN { inhdr = 1; printf "" > H; printf "" > B }
        inhdr { sub(/\r$/, ""); if ($0 == "") { inhdr = 0; next } print > H; next }
        { print > B }' "$WORK/raw"
    code="$(awk 'NR == 1 { print $2; exit }' "$WORK/hdr")"
    remaining="$(header X-RateLimit-Remaining)"
    reset="$(header X-RateLimit-Reset)"
    [[ "$remaining" =~ ^[0-9]+$ ]] && RL_REMAINING="$remaining"
    [[ "$reset" =~ ^[0-9]+$ ]] && RL_RESET="$reset"
    case "$code" in
        2??)
            jq -e . "$WORK/body" >/dev/null 2>&1 || return 1
            mv "$WORK/body" "$WORK/cache/$key.body"
            header ETag > "$WORK/cache/$key.etag"
            return 0 ;;
        304)
            [ -n "$etag" ] || return 1
            return 0 ;;
        403|429)
            if [ "$remaining" = "0" ]; then return 3; fi
            return 1 ;;
        *) return 1 ;;
    esac
}

# fetch_list <kind> <endpoint> <array-field> <projection> — every page of a
# list endpoint, each item through <projection>, into $WORK/<kind>.json. A page
# shorter than PER_PAGE is the last one.
fetch_list() {
    local kind="$1" endpoint="$2" field="$3" proj="$4" page=1 n rc sep='?'
    case "$endpoint" in *\?*) sep='&' ;; esac
    : > "$WORK/$kind.items"
    while :; do
        rc=0
        rest_get "$kind.$page" "$endpoint${sep}per_page=$PER_PAGE&page=$page" || rc=$?
        [ "$rc" -eq 0 ] || return "$rc"
        jq -c --arg f "$field" ".[\$f][]? | $proj" "$WORK/cache/$kind.$page.body" >> "$WORK/$kind.items" || return 1
        n="$(jq --arg f "$field" '.[$f] | length' "$WORK/cache/$kind.$page.body")" || return 1
        [ "$n" -ge "$PER_PAGE" ] || break
        page=$(( page + 1 ))
        [ "$page" -le 50 ] || break   # 5,000 entries: far past any real head
    done
    jq -s '.' "$WORK/$kind.items" > "$WORK/$kind.json"
}

# fetch_checks — the head's check runs + commit statuses (every page) into $WORK.
fetch_checks() {
    fetch_list runs "repos/$REPO/commits/$SHA/check-runs?filter=all" check_runs \
        '{id, name, status, conclusion, started_at, completed_at, check_suite: {id: .check_suite.id}}' || return
    fetch_list stats "repos/$REPO/commits/$SHA/status" statuses \
        '{id, context, state, created_at, updated_at}' || return
}

# fetch_pr — head SHA, state (open | closed | merged), live labels + body (the
# disposition markers).
fetch_pr() {
    local rc=0
    rest_get pr "repos/$REPO/pulls/$PR" || rc=$?
    [ "$rc" -eq 0 ] || return "$rc"
    jq '{head: .head.sha, state: (if .merged == true then "merged" else .state end), labels: [.labels[]?.name], body: (.body // "")}' \
        "$WORK/cache/pr.body" > "$WORK/pr.json.new" || return 1
    mv "$WORK/pr.json.new" "$WORK/pr.json"
}

# poll_wait — sleep before the next poll, sized by the REST budget left: the
# base interval; x4 below RL_LOW; below RL_STOP, until the rate-limit reset
# (never past the wait budget — the deadline check then reports the timeout).
poll_wait() {
    local wait="$POLL" now left
    if [ -n "$RL_REMAINING" ] && [ "$RL_REMAINING" -lt "$RL_STOP" ]; then
        now="$(date +%s)"
        wait=$(( ${RL_RESET:-0} - now + 5 ))
        [ "$wait" -ge "$POLL" ] || wait="$POLL"
        left=$(( DEADLINE - now ))
        [ "$left" -ge 1 ] || left=1
        [ "$wait" -le "$left" ] || wait="$left"
        echo "::warning title=All checks green — REST budget low::GITHUB_TOKEN has $RL_REMAINING REST calls left; pausing polls ${wait}s until the rate-limit reset (this check stays pending meanwhile)."
    elif [ -n "$RL_REMAINING" ] && [ "$RL_REMAINING" -lt "$RL_LOW" ]; then
        wait=$(( POLL * 4 ))
        echo "all-checks-green: REST budget low ($RL_REMAINING calls left) — poll interval widened to ${wait}s."
    fi
    "$SLEEP_BIN" "$wait"
}

# pr_superseded — end the run when the PR moved on. A PR that is no longer open
# exits 0: there is no merge left to gate, and a red here would sit on the
# merged head as a false gate escape. A moved head exits 1 (void; the new head
# gets its own run).
pr_superseded() {
    local head state
    head="$(jq -r '.head' "$WORK/pr.json")"
    state="$(jq -r '.state' "$WORK/pr.json")"
    if [ "$state" != "open" ]; then
        echo "::notice title=All checks green — verdict moot::verdict moot: PR #$PR is $state; there is no merge left to gate."
        echo "all-checks-green: verdict moot: PR is $state."
        exit 0
    fi
    if [ "$head" != "$SHA" ]; then
        echo "::error title=All checks green — superseded::PR #$PR head moved ${SHA:0:12} -> ${head:0:12}; this run's verdict is void (the new head gets its own run)."
        exit 1
    fi
}

# judge — assemble the poll's document and set $v (verdict object) + $verdict.
judge() {
    jq -n --slurpfile r "$WORK/runs.json" --slurpfile s "$WORK/stats.json" --slurpfile p "$WORK/pr.json" \
        '{check_runs: $r[0], statuses: $s[0], labels: $p[0].labels, body: $p[0].body}' > "$WORK/doc.json" \
        || die "cannot assemble poll $poll"
    v="$(evaluate "$WORK/doc.json" "$REQ")" || die "cannot evaluate poll $poll"
    verdict="$(jq -r .verdict <<<"$v")"
}

START="$(date +%s)"
DEADLINE=$(( START + MAX_WAIT ))
poll=0
api_fail=0
settle_fp=""
echo "all-checks-green: PR #$PR head ${SHA:0:12} — every other check must end green (poll ${POLL}s, settle ${SETTLE}s, budget ${MAX_WAIT}s)."
while :; do
    poll=$(( poll + 1 ))
    pr_fresh=0
    if [ $(( (poll - 1) % PR_REFRESH )) -eq 0 ]; then pr_fresh=1; fi
    fetch_rc=0
    fetch_checks || fetch_rc=$?
    if [ "$fetch_rc" -eq 0 ] && [ "$pr_fresh" -eq 1 ]; then fetch_pr || fetch_rc=$?; fi
    if [ "$fetch_rc" -eq 3 ]; then
        # Rate-limited: not an API failure — wait for the reset, within budget.
        RL_REMAINING=0
        if [ "$(date +%s)" -ge "$DEADLINE" ]; then
            echo "::error title=All checks green — timed out::GITHUB_TOKEN REST budget exhausted and the ${MAX_WAIT}s wait budget is spent. Re-run the job."
            exit 1
        fi
        echo "all-checks-green: poll $poll — rate-limited by GitHub."
        poll_wait
        continue
    fi
    if [ "$fetch_rc" -ne 0 ]; then
        api_fail=$(( api_fail + 1 ))
        if [ "$api_fail" -ge "$MAX_API_FAIL" ]; then
            echo "::error title=All checks green — GitHub API unavailable::$api_fail consecutive API failures; failing closed. Re-run the job."
            exit 2
        fi
        echo "all-checks-green: poll $poll — GitHub API error ($api_fail/$MAX_API_FAIL): $(head -n 1 "$WORK/raw.err" 2>/dev/null || :) — backing off."
        "$SLEEP_BIN" $(( POLL * 2 > 300 ? 300 : POLL * 2 ))
        continue
    fi
    api_fail=0
    if [ "$pr_fresh" -eq 1 ]; then pr_superseded; fi
    judge
    # A terminal verdict is always judged on a fresh PR read (head, labels, body).
    if [ "$verdict" != "pending" ] && [ "$pr_fresh" -eq 0 ]; then
        if ! fetch_pr; then
            echo "all-checks-green: poll $poll — PR read failed, re-polling."
            "$SLEEP_BIN" "$POLL"
            continue
        fi
        pr_superseded
        judge
    fi
    elapsed=$(( $(date +%s) - START ))
    case "$verdict" in
        failure)
            report "$v"
            echo "::error title=All checks green — blocking red::$(jq -r '.failing | join("; ")' <<<"$v")"
            exit 1
            ;;
        success)
            fp="$(jq -r .fingerprint <<<"$v")"
            if [ -n "$settle_fp" ] && [ "$fp" = "$settle_fp" ]; then
                report "$v"
                echo "all-checks-green: PASS — every other check is terminal and green (settled, ${elapsed}s)."
                exit 0
            fi
            settle_fp="$fp"
            echo "all-checks-green: poll $poll (+${elapsed}s) — all green; re-polling in ${SETTLE}s for late checks."
            # A green set that has not settled may still take its settle re-poll
            # past the budget (two settle windows of grace); a set that keeps
            # changing beyond that is a timeout like any other.
            if [ "$(date +%s)" -ge $(( DEADLINE + 2 * SETTLE )) ]; then
                report "$v"
                echo "::error title=All checks green — timed out::budget ${MAX_WAIT}s spent before the green set settled. Re-run the job."
                exit 1
            fi
            "$SLEEP_BIN" "$SETTLE"
            continue
            ;;
    esac
    settle_fp=""
    echo "all-checks-green: poll $poll (+${elapsed}s) — waiting on: $(jq -r '(.pending + [.absent[] | "\(.) (not reported)"]) | .[0:6] | join("; ")' <<<"$v")$(jq -r 'if ((.pending | length) + (.absent | length)) > 6 then " …" else "" end' <<<"$v")"
    if [ "$(date +%s)" -ge "$DEADLINE" ]; then
        report "$v"
        echo "::error title=All checks green — timed out::still pending after ${MAX_WAIT}s — a timeout is red (fail-closed). Re-run the job once the pending checks finish."
        exit 1
    fi
    poll_wait
done
