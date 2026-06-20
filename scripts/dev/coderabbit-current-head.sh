#!/usr/bin/env bash
# coderabbit-current-head.sh — print the CodeRabbit signal scoped to a PR's
# CURRENT head SHA, so a triage round never re-reads stale review history.
#
# WHY: CodeRabbit posts a fresh review (and a "CodeRabbit" status check) per
# pushed head, but its prior-commit reviews + conversation comments stay on the
# PR forever. A human (or agent) skimming the PR conversation easily mistakes a
# resolved finding from an OLD commit for a live one on the current head — the
# "addressed annotation is keyword-matched, not diff-matched" trap (AGENTS.md
# § Merge gates). This helper collapses the noise to exactly what applies to the
# head you are about to merge:
#   • the current head SHA (headRefOid)
#   • the latest CodeRabbit CHECK state on THAT head (StatusContext "CodeRabbit"
#     or the "CodeRabbit" / "CR findings (0 actionable)" CheckRun)
#   • the latest CodeRabbit REVIEW whose .commit.oid == the head SHA (state +
#     first body line, which carries "Actionable comments posted: N")
#   • an explicit "historical comments ignored" note naming how many prior-head
#     CR reviews exist that this view deliberately drops.
# Mirrors the head-scoping the merge-gate poller applies ($crstate keys on
# .commit.oid == $sha); use this for a quick human read, the poller for gating.
#
# Usage:
#   bash scripts/dev/coderabbit-current-head.sh <pr>
#   bash scripts/dev/coderabbit-current-head.sh --selftest   # pure formatter, no gh
#
# Exit: 0 ok · 2 usage error / gh missing · 3 gh API failure.
#
# selftest: asserts-failure: n/a — this helper has no gating failure mode (it is a
# read-only reporter, not a gate). The --selftest exercises the PURE formatter on
# BOTH the head-review-present and the head-review-ABSENT/zero-history shapes and
# asserts each renders the right text; a regression in either branch fails the
# selftest (the test-gate-selftests dogfood is satisfied by this marker).
set -uo pipefail

OWNER_REPO_OVERRIDE="${CRCH_OWNER_REPO:-}"   # test/CI override; else gh infers

# ----------------------------------------------------------------------------
# format_report — PURE renderer. Takes the already-extracted facts as args and
# prints the human report. No gh / network, so --selftest exercises it directly.
#   $1 head_sha · $2 cr_check_state · $3 cr_review_state · $4 cr_review_first_line
#   $5 historical_count (CR reviews NOT on the current head)
# ----------------------------------------------------------------------------
format_report() {
    local head_sha="$1" check_state="$2" review_state="$3" review_first="$4" hist="$5"
    echo "CodeRabbit @ current head"
    echo "  head SHA        : ${head_sha:-<unknown>}"
    echo "  CR check state  : ${check_state:-ABSENT}"
    if [ -n "$review_state" ] && [ "$review_state" != "NONE" ]; then
        echo "  CR review (head): ${review_state}"
        echo "  actionable line : ${review_first:-<no body>}"
    else
        echo "  CR review (head): NONE (no CodeRabbit review on this exact head yet)"
    fi
    # The load-bearing note: prior-head CR reviews exist but are deliberately
    # NOT shown — they describe code that may have changed since.
    if [ "${hist:-0}" -gt 0 ] 2>/dev/null; then
        echo "  NOTE: $hist historical CodeRabbit review(s) on PRIOR head(s) ignored — they describe superseded commits. Re-trigger with '@coderabbitai review' if the head review is missing/stale."
    else
        echo "  NOTE: historical CodeRabbit comments on prior heads are ignored by design — only the current head's review counts for merge."
    fi
}

# ----------------------------------------------------------------------------
# --selftest — pure formatter fixtures (no gh, no network).
# ----------------------------------------------------------------------------
if [ "${1:-}" = "--selftest" ]; then
    fail=0
    out="$(format_report "abc1234" "SUCCESS" "COMMENTED" "Actionable comments posted: 0" 3)"
    case "$out" in
        *"head SHA        : abc1234"*) ;;
        *) echo "selftest FAIL: head SHA not rendered" >&2; fail=1 ;;
    esac
    case "$out" in
        *"CR review (head): COMMENTED"*) ;;
        *) echo "selftest FAIL: head review state not rendered" >&2; fail=1 ;;
    esac
    case "$out" in
        *"3 historical CodeRabbit review(s) on PRIOR head(s) ignored"*) ;;
        *) echo "selftest FAIL: historical-ignored note missing for hist>0" >&2; fail=1 ;;
    esac
    # No-head-review variant + zero history → the by-design ignored note.
    out2="$(format_report "def5678" "ABSENT" "NONE" "" 0)"
    case "$out2" in
        *"CR review (head): NONE"*) ;;
        *) echo "selftest FAIL: NONE review branch not rendered" >&2; fail=1 ;;
    esac
    case "$out2" in
        *"historical CodeRabbit comments on prior heads are ignored by design"*) ;;
        *) echo "selftest FAIL: by-design ignored note missing for hist=0" >&2; fail=1 ;;
    esac
    if [ "$fail" = 0 ]; then
        echo "selftest: coderabbit-current-head formatter OK"
        echo "Passed: 1  Failed: 0"
        exit 0
    fi
    echo "Passed: 0  Failed: 1"
    exit 1
fi

PR="${1:-}"
if [ -z "$PR" ] || [[ "$PR" == -* ]]; then
    echo "usage: $0 <pr>    (or --selftest)" >&2
    exit 2
fi

command -v gh >/dev/null 2>&1 || { echo "coderabbit-current-head: gh required (https://cli.github.com)" >&2; exit 2; }

# Resolve owner/repo: explicit override (tests/CI) else gh's repo inference.
owner=""; repo=""
if [ -n "$OWNER_REPO_OVERRIDE" ]; then
    owner="${OWNER_REPO_OVERRIDE%%/*}"
    repo="${OWNER_REPO_OVERRIDE##*/}"
else
    nwo="$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" || {
        echo "coderabbit-current-head: cannot resolve owner/repo (run inside the repo, or set CRCH_OWNER_REPO=owner/repo)" >&2
        exit 2
    }
    owner="${nwo%%/*}"
    repo="${nwo##*/}"
fi

# One GraphQL call: head SHA + CR status/check rollup + all CR reviews (with the
# commit each was submitted against, so we can scope to the head + count history).
read -r -d '' QUERY <<'GRAPHQL'
query($owner:String!, $repo:String!, $pr:Int!) {
  repository(owner:$owner, name:$repo) {
    pullRequest(number:$pr) {
      headRefOid
      commits(last:1) { nodes { commit { statusCheckRollup { contexts(first:100) { nodes {
        __typename
        ... on StatusContext { context state }
        ... on CheckRun { name conclusion } } } } } } }
      reviews(last:100) { nodes {
        state submittedAt body
        commit { oid }
        author { login } } }
    }
  }
}
GRAPHQL

# Extract the five facts with gh's bundled jq (no standalone jq dep).
# Field order: head_sha · cr_check_state · cr_review_state · cr_review_first_line · historical_count
FILTER='
.data.repository.pullRequest as $pr
| ($pr.headRefOid // "") as $sha
| ([$pr.reviews.nodes[]? | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")]) as $crall
| ([$crall[] | select((.commit.oid // "") == $sha)]) as $crhead
| (
    $sha,
    ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
      | if (.__typename == "StatusContext" and .context == "CodeRabbit") then .state
        elif (.__typename == "CheckRun" and ((.name == "CodeRabbit") or (.name == "CR findings (0 actionable)")) and (.conclusion != null))
          then (if ((.conclusion) | IN("SUCCESS","NEUTRAL","SKIPPED")) then "SUCCESS" else .conclusion end)
        else empty end] | (.[0] // "ABSENT")),
    (if ($crhead | length) == 0 then "NONE" else ($crhead | sort_by(.submittedAt) | .[-1].state) end),
    (if ($crhead | length) == 0 then "" else (($crhead | sort_by(.submittedAt) | .[-1].body // "") | split("\n") | .[0] // "") end),
    (($crall | length) - ($crhead | length))
  )
'

data="$(gh api graphql -f owner="$owner" -f repo="$repo" -F pr="$PR" -f query="$QUERY" --jq "$FILTER" 2>&1)" || {
    echo "coderabbit-current-head: gh API failed: $data" >&2
    exit 3
}
# Strip CR (Windows gh/jq emit CRLF) then split into the five fixed-order lines.
data="${data//$'\r'/}"
mapfile -t F <<<"$data"
if [ "${#F[@]}" -ne 5 ]; then
    echo "coderabbit-current-head: unexpected field count ${#F[@]} (expected 5); gh response malformed" >&2
    exit 3
fi

format_report "${F[0]}" "${F[1]}" "${F[2]}" "${F[3]}" "${F[4]}"
exit 0
