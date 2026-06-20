#!/usr/bin/env bash
# pr-blocked-why.sh — explain WHY a green-looking PR is still BLOCKED.
#
# WHY (PR-1 § green-pr-blocked-no-merge-signal):
#   A PR can show every CI check green yet sit at mergeStateStatus=BLOCKED with no
#   obvious reason — GitHub surfaces the *fact* of the block but not the precise
#   cause. The orchestrator then guesses (re-run CI? wait? admin-merge?). This
#   script names the exact blocker class for a MERGEABLE+BLOCKED PR via one
#   `gh api graphql` call, so the next action is unambiguous:
#     * unresolved review threads          → resolve them (lists each: author + path)
#     * skipped / pending / failing required checks → the named check(s) must finish/pass
#     * a required-review shortfall          → reviewDecision != APPROVED (who/what is owed)
#     * a required context that never ran    → absent from the head rollup (fail-closed)
#
#   It is READ-ONLY and diagnostic — it never merges. It complements safe-merge.sh
#   (which decides arm/refuse) by explaining a refuse / a stuck BLOCKED state.
#
# Usage:
#   bash scripts/dev/pr-blocked-why.sh <pr>
#   bash scripts/dev/pr-blocked-why.sh --selftest      # classifier fixtures (no gh)
#
# Required-context ground truth is read from project.config.json
# (branch_protection.required_contexts), UTF-8-safe via jq — same source as
# merge-gates.sh. Override with PR_BLOCKED_WHY_REQUIRED_CONTEXTS (newline/comma).
#
# Env knobs:
#   PR_BLOCKED_WHY_REQUIRED_CONTEXTS — override the required-context set.
#   PR_BLOCKED_WHY_CONFIG_FILE       — override project.config.json path.
#   PR_BLOCKED_WHY_STUB_JSON         — TEST-ONLY: a JSON blob used in place of the
#                                      `gh api graphql` response (the pullRequest
#                                      object). Lets --selftest run with no gh.
#   ORCH_USER                        — orchestrator login (self-comment filtering;
#                                      defaults to `gh api user` when unset).
#
# Exit:
#   0 — ran; a human-readable blocker report printed (even "no blocker found").
#   2 — usage / dependency error (gh or jq missing, bad args).
#   3 — gh API call failed.
#
# selftest: asserts-failure
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source merge-gates.sh ONLY for the single-source block allow-list constant, so
# a non-required-but-meant-to-block RED check is classified identically to the
# merge gate. Side-effect-free when sourced.
# shellcheck source=agents/scripts/core/merge-gates.sh
source "$REPO_ROOT/agents/scripts/core/merge-gates.sh" 2>/dev/null || true
ALLOW_RE="${MERGE_GATES_BLOCK_ALLOWLIST_RE:-Coverage|Sanitizer|Perf PR-fast|Android security gate|Fuzz smoke}"

read_required_contexts() {
    if [ -n "${PR_BLOCKED_WHY_REQUIRED_CONTEXTS+x}" ]; then
        printf '%s\n' "${PR_BLOCKED_WHY_REQUIRED_CONTEXTS//,/$'\n'}"
        return 0
    fi
    local config_file="${PR_BLOCKED_WHY_CONFIG_FILE:-$REPO_ROOT/project.config.json}"
    if [ -f "$config_file" ] && command -v jq >/dev/null 2>&1; then
        jq -r '.branch_protection.required_contexts[]? // empty' "$config_file" 2>/dev/null || true
    fi
}

# ----------------------------------------------------------------------------
# classify_blockers <pr_json> <req-newline-list> <orch_user> — the PURE core,
# fully testable with no `gh`. Reads the pullRequest object and emits a
# human-readable, multi-line blocker report on stdout. Exit 0 always (a parse
# failure prints a fail-closed "could not evaluate" line, never a silent pass).
#
# Classes (each printed when present):
#   THREADS  — unresolved, non-outdated review threads (path + first author)
#   CHECKS   — required-or-allow-listed checks that are FAILING or PENDING
#   ABSENT   — required contexts absent from the head rollup (never ran)
#   REVIEW   — reviewDecision is not APPROVED/null (a required review is owed)
#   STATE    — mergeStateStatus / state (context line, always printed)
# ----------------------------------------------------------------------------
classify_blockers() {
    local pr_json="$1" req_list="$2" orch="$3"
    local req_json='[]'
    if [ -n "$req_list" ]; then
        req_json=$(printf '%s\n' "$req_list" | jq -R . | jq -sc 'map(select(length > 0))') || req_json='[]'
    fi

    local out
    if ! out=$(printf '%s' "$pr_json" | jq -r \
        --argjson req "$req_json" \
        --arg allow "$ALLOW_RE" \
        --arg orch "$orch" '
        . as $pr
        | ($pr.mergeStateStatus // "UNKNOWN") as $mss
        | ($pr.state // "UNKNOWN") as $state
        | ($pr.reviewDecision // "NONE") as $rd
        | (($pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes) // []) as $ctx
        | ([$ctx[] | {
              name: (if .__typename == "CheckRun" then (.name // "") else (.context // "") end),
              green: (if .__typename == "CheckRun"
                      then (.status == "COMPLETED"
                            and ((.conclusion // "") | ascii_upcase | IN("SUCCESS","NEUTRAL","SKIPPED")))
                      else ((.state // "") | ascii_upcase | . == "SUCCESS") end),
              pending: (if .__typename == "CheckRun" then (.status != "COMPLETED")
                        else ((.state // "") | ascii_upcase | IN("PENDING","EXPECTED")) end),
              gating: (.isRequired == true
                       or ((if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
                           | (test($allow; "i") and (ascii_downcase | contains("advisory") | not))))
           }]) as $rows
        | ([$rows[].name]) as $present
        | ([$rows[] | select(.gating and (.green | not) and (.pending | not)) | .name]) as $failing
        | ([$rows[] | select(.gating and .pending) | .name]) as $pendingChecks
        | ([$req[] | select(. as $n | ($present | any(. == $n)) | not)]) as $absentReq
        | ([$pr.reviewThreads.nodes[]?
             | select(.isResolved == false and .isOutdated == false)
             | { path: (.path // "(conversation)"),
                 author: ((.comments.nodes // []) | (.[0].author.login // "?")),
                 isBot: ((.comments.nodes // []) | any(.author.__typename == "Bot")) }]) as $threads
        | "STATE   — state=\($state) mergeStateStatus=\($mss) reviewDecision=\($rd)",
          (if ($failing | length) > 0
             then "CHECKS  — \($failing | length) gating check(s) FAILING: \($failing | join(", "))"
             else empty end),
          (if ($pendingChecks | length) > 0
             then "CHECKS  — \($pendingChecks | length) gating check(s) PENDING (not yet terminal): \($pendingChecks | join(", "))"
             else empty end),
          (if ($absentReq | length) > 0
             then "ABSENT  — \($absentReq | length) required context(s) never ran on the head: \($absentReq | join(", "))"
             else empty end),
          (if ($rd != "APPROVED" and $rd != "NONE" and $rd != null)
             then "REVIEW  — reviewDecision=\($rd) (a required review is owed / changes requested)"
             else empty end),
          (if ($threads | length) > 0
             then "THREADS — \($threads | length) unresolved review thread(s):"
             else empty end),
          ($threads[]? | "          - \(.path) (by \(.author)\(if .isBot then ", bot" else "" end))"),
          (if (($failing | length) == 0 and ($pendingChecks | length) == 0
               and ($absentReq | length) == 0 and ($threads | length) == 0
               and ($rd == "APPROVED" or $rd == "NONE" or $rd == null))
             then "OK      — no gating check / absent-required / unresolved-thread / review blocker found. A BLOCKED state here is likely a stale GitHub mergeability cache (branch-protection summary-only) — a REST squash-merge or safe-merge.sh should proceed."
             else empty end)
        ' 2>/dev/null); then
        # Fail-closed: a jq parse/runtime error on a malformed PR object must NOT
        # read as a silent "no blocker" — emit an explicit cannot-evaluate line so
        # the caller never mistakes a broken response for a clean PR.
        echo "ERROR   — could not evaluate the PR object (malformed / unexpected shape); treat as BLOCKED until re-fetched (fail-closed)."
        return 0
    fi
    printf '%s\n' "$out"
}

GRAPHQL='
query($owner:String!, $repo:String!, $pr:Int!) {
  repository(owner:$owner, name:$repo) {
    pullRequest(number:$pr) {
      state
      mergeStateStatus
      reviewDecision
      commits(last:1) { nodes { commit { statusCheckRollup { contexts(first:100) { nodes {
        __typename
        ... on CheckRun { name status conclusion isRequired(pullRequestNumber:$pr) }
        ... on StatusContext { context state isRequired(pullRequestNumber:$pr) }
      } } } } } }
      reviewThreads(first:100) { nodes {
        isResolved isOutdated path
        comments(first:1) { nodes { author { login __typename } } }
      } }
    }
  }
}'

run_selftest() {
    local fails=0 out

    # CASE 1 — an unresolved review thread is reported.
    local j1
    j1='{"state":"OPEN","mergeStateStatus":"BLOCKED","reviewDecision":"APPROVED",
      "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
        {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS","isRequired":true}]}}}}]},
      "reviewThreads":{"nodes":[
        {"isResolved":false,"isOutdated":false,"path":"src/foo.cpp",
         "comments":{"nodes":[{"author":{"login":"alice","__typename":"User"}}]}}]}}'
    out=$(classify_blockers "$j1" "Windows + MSVC" orch)
    if printf '%s' "$out" | grep -q 'THREADS' && printf '%s' "$out" | grep -q 'src/foo.cpp'; then
        echo "selftest CASE1 PASS — unresolved thread reported with path"
    else
        echo "selftest CASE1 FAIL — should report the unresolved thread (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 2 — a FAILING gating check is named.
    local j2
    j2='{"state":"OPEN","mergeStateStatus":"BLOCKED","reviewDecision":"APPROVED",
      "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
        {"__typename":"StatusContext","context":"Windows + MSVC","state":"FAILURE","isRequired":true}]}}}}]},
      "reviewThreads":{"nodes":[]}}'
    out=$(classify_blockers "$j2" "Windows + MSVC" orch)
    if printf '%s' "$out" | grep -q 'CHECKS' && printf '%s' "$out" | grep -q 'FAILING'; then
        echo "selftest CASE2 PASS — failing required check named"
    else
        echo "selftest CASE2 FAIL — should name the failing check (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 3 — an ABSENT required context is reported (never ran).
    local j3
    j3='{"state":"OPEN","mergeStateStatus":"BLOCKED","reviewDecision":"APPROVED",
      "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
        {"__typename":"StatusContext","context":"Linux + Clang","state":"SUCCESS","isRequired":true}]}}}}]},
      "reviewThreads":{"nodes":[]}}'
    out=$(classify_blockers "$j3" "Windows + MSVC" orch)
    if printf '%s' "$out" | grep -q 'ABSENT' && printf '%s' "$out" | grep -q 'Windows + MSVC'; then
        echo "selftest CASE3 PASS — absent required context reported"
    else
        echo "selftest CASE3 FAIL — should report the absent required context (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 4 — a review shortfall (reviewDecision REVIEW_REQUIRED) is reported.
    local j4
    j4='{"state":"OPEN","mergeStateStatus":"BLOCKED","reviewDecision":"REVIEW_REQUIRED",
      "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
        {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS","isRequired":true}]}}}}]},
      "reviewThreads":{"nodes":[]}}'
    out=$(classify_blockers "$j4" "Windows + MSVC" orch)
    if printf '%s' "$out" | grep -q 'REVIEW' && printf '%s' "$out" | grep -q 'REVIEW_REQUIRED'; then
        echo "selftest CASE4 PASS — review shortfall reported"
    else
        echo "selftest CASE4 FAIL — should report the review shortfall (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 5 — a genuinely-clean PR reports the OK / stale-cache line.
    local j5
    j5='{"state":"OPEN","mergeStateStatus":"BLOCKED","reviewDecision":"APPROVED",
      "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
        {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS","isRequired":true}]}}}}]},
      "reviewThreads":{"nodes":[]}}'
    out=$(classify_blockers "$j5" "Windows + MSVC" orch)
    if printf '%s' "$out" | grep -q 'OK' && printf '%s' "$out" | grep -qi 'stale'; then
        echo "selftest CASE5 PASS — clean PR reports the stale-cache OK line"
    else
        echo "selftest CASE5 FAIL — should report OK/stale-cache (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 6 — a PENDING allow-listed (non-required) check is reported as PENDING.
    local j6
    j6='{"state":"OPEN","mergeStateStatus":"BLOCKED","reviewDecision":"APPROVED",
      "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
        {"__typename":"CheckRun","name":"Sanitizer","status":"IN_PROGRESS","conclusion":null,"isRequired":false},
        {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS","isRequired":true}]}}}}]},
      "reviewThreads":{"nodes":[]}}'
    out=$(classify_blockers "$j6" "Windows + MSVC" orch)
    if printf '%s' "$out" | grep -q 'PENDING' && printf '%s' "$out" | grep -q 'Sanitizer'; then
        echo "selftest CASE6 PASS — pending allow-listed Sanitizer reported"
    else
        echo "selftest CASE6 FAIL — should report pending Sanitizer (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 7 — asserts-failure: a MALFORMED PR object must fail closed (emit the
    # explicit ERROR/BLOCKED line, never a silent OK that reads as a clean PR).
    out=$(classify_blockers 'this is not json' "" orch)
    if printf '%s' "$out" | grep -q 'ERROR' && printf '%s' "$out" | grep -qi 'fail-closed'; then
        echo "selftest CASE7 PASS — malformed PR object fails closed (no silent OK)"
    else
        echo "selftest CASE7 FAIL — malformed input should fail closed (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    if [ "$fails" -eq 0 ]; then
        echo "PASS — pr-blocked-why --selftest (7/7)"
        return 0
    fi
    echo "FAIL — pr-blocked-why --selftest ($fails failing case(s))" >&2
    return 1
}

main() {
    local arg="${1:-}"
    case "$arg" in
        --selftest) run_selftest; exit $? ;;
        ""|-h|--help)
            sed -n '2,40p' "${BASH_SOURCE[0]}"
            [ -z "$arg" ] && exit 2 || exit 0 ;;
    esac

    local pr="$arg"
    if ! [[ "$pr" =~ ^[0-9]+$ ]]; then
        echo "pr-blocked-why: <pr> must be a PR number (got: '$pr')" >&2
        exit 2
    fi
    command -v jq >/dev/null 2>&1 || { echo "pr-blocked-why: jq required" >&2; exit 2; }

    local orch="${ORCH_USER:-}"
    local pr_json
    if [ -n "${PR_BLOCKED_WHY_STUB_JSON:-}" ]; then
        pr_json="$PR_BLOCKED_WHY_STUB_JSON"
    else
        command -v gh >/dev/null 2>&1 || { echo "pr-blocked-why: gh required" >&2; exit 2; }
        [ -n "$orch" ] || orch=$(gh api user --jq .login 2>/dev/null) || orch=""
        local nwo owner repo
        nwo=$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null) || nwo=""
        owner="${nwo%%/*}"; repo="${nwo##*/}"
        if [ -z "$owner" ] || [ -z "$repo" ]; then
            echo "pr-blocked-why: cannot resolve owner/repo (run inside the repo)." >&2
            exit 2
        fi
        if ! pr_json=$(gh api graphql -f owner="$owner" -f repo="$repo" -F pr="$pr" \
                          -f query="$GRAPHQL" --jq '.data.repository.pullRequest' 2>&1); then
            echo "pr-blocked-why: gh api graphql failed: $pr_json" >&2
            exit 3
        fi
    fi

    echo "PR #$pr — blocker analysis:"
    local req_list
    req_list=$(read_required_contexts)
    classify_blockers "$pr_json" "$req_list" "$orch"
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
