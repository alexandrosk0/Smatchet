#!/usr/bin/env bash
# scripts/dev/merge-gates.sh
# ----------------------------------------------------------------------------
# Merge-gates poller for the orchestrator + git-janitor ship-loop.
#
# Polls three conditions on a PR via one `gh api graphql` call:
#   1. CI — every required check passes (CheckRun terminal SUCCESS/NEUTRAL/SKIPPED;
#      StatusContext state == SUCCESS)
#   2. CodeRabbit — latest review on current headRefOid is not CHANGES_REQUESTED;
#      zero unresolved non-outdated review threads contain a CodeRabbit comment
#   3. User comments — zero unresolved non-outdated review threads with any
#      non-bot non-self comment; zero conversation-tab comments from non-bot
#      non-self authors
#
# Plus: pullRequest.state == OPEN, reviewDecision in {APPROVED, null},
# all connection pageInfo.hasNextPage == false.
#
# Usage:
#   source scripts/dev/merge-gates.sh
#   poll_merge_gates <owner> <repo> <pr_number>
# OR:
#   scripts/dev/merge-gates.sh <owner> <repo> <pr_number>
#
# Env knobs:
#   ORCH_USER                    — orchestrator GitHub login (required)
#   MERGE_GATES_POLL_INTERVAL    — seconds between polls (default 60)
#   MERGE_GATES_MAX_POLLS        — max poll count (default 60)
#   MERGE_GATES_TIMEOUT_SECONDS  — wall-clock budget (default 3600)
#   MERGE_GATES_QUERY_FILE       — override GraphQL document path
#
# Return codes (poll_merge_gates):
#   0 — gates passed
#   1 — gates still blocked at MAX_POLLS
#   2 — timeout (≥MERGE_GATES_TIMEOUT_SECONDS wall-clock)
#   3 — gh API down (3 consecutive failures)
#   4 — PR closed or merged externally
#   5 — pagination overflow (any connection has more pages)
#
# Return codes (gh_pr_ready_idempotent):
#   0 — PR is now ready (or already was)
#   6 — unknown failure (caller halts; do not auto-merge)
# ----------------------------------------------------------------------------

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_QUERY_FILE="$SCRIPT_DIR/merge-gates.graphql"

# Source prompt shim so `ask_user_question` is callable from the caller's
# integration flow. Lazy — only if available.
if [ -f "$SCRIPT_DIR/merge-gates-prompt.sh" ]; then
    # shellcheck source=scripts/dev/merge-gates-prompt.sh
    source "$SCRIPT_DIR/merge-gates-prompt.sh"
fi

# ----------------------------------------------------------------------------
# poll_merge_gates <owner> <repo> <pr_number>
# ----------------------------------------------------------------------------
poll_merge_gates() {
    local owner="${1:?poll_merge_gates: owner required}"
    local repo="${2:?poll_merge_gates: repo required}"
    local prNumber="${3:?poll_merge_gates: pr_number required}"

    if [ -z "${ORCH_USER:-}" ]; then
        echo "poll_merge_gates: ORCH_USER not set (run: ORCH_USER=\$(gh api user --jq .login))" >&2
        return 3
    fi

    local POLL_INTERVAL="${MERGE_GATES_POLL_INTERVAL:-60}"
    local MAX_POLLS="${MERGE_GATES_MAX_POLLS:-60}"
    local TIMEOUT_SECONDS="${MERGE_GATES_TIMEOUT_SECONDS:-3600}"
    local QUERY_FILE="${MERGE_GATES_QUERY_FILE:-$DEFAULT_QUERY_FILE}"

    if [ ! -f "$QUERY_FILE" ]; then
        echo "poll_merge_gates: query file not found: $QUERY_FILE" >&2
        return 3
    fi

    local start gh_fails=0
    start=$(date +%s)

    local p
    for ((p=0; p<MAX_POLLS; p++)); do
        local data
        if ! data=$(gh api graphql \
                       -f owner="$owner" \
                       -f repo="$repo" \
                       -F pr="$prNumber" \
                       -f query=@"$QUERY_FILE" 2>&1); then
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gh failed ($gh_fails/3): $data"
            if [ "$gh_fails" -ge 3 ]; then
                echo "GH_API_DOWN"
                return 3
            fi
            if [ "$p" -lt $((MAX_POLLS-1)) ]; then
                sleep "$POLL_INTERVAL"
            fi
            continue
        fi
        gh_fails=0

        local pr
        pr=$(jq '.data.repository.pullRequest' <<<"$data")

        # PR state early-exit
        local pr_state
        pr_state=$(jq -r '.state' <<<"$pr")
        if [ "$pr_state" != "OPEN" ]; then
            echo "PR_$pr_state"
            return 4
        fi

        local head_sha
        head_sha=$(jq -r '.headRefOid' <<<"$pr")

        # Pagination overflow
        local overflow
        overflow=$(jq '
            ((.commits.nodes[0].commit.statusCheckRollup.contexts.pageInfo.hasNextPage // false)
             or (.reviews.pageInfo.hasNextPage // false)
             or (.reviewThreads.pageInfo.hasNextPage // false)
             or (.comments.pageInfo.hasNextPage // false)
             or (any(.reviewThreads.nodes[]?; .comments.pageInfo.hasNextPage // false)))
        ' <<<"$pr")
        if [ "$overflow" = "true" ]; then
            echo "PAGINATION_OVERFLOW"
            return 5
        fi

        # CI — required-only
        local ctx='((.commits.nodes[0].commit.statusCheckRollup.contexts.nodes) // [])'
        local ci_total ci_fail ci_pend
        ci_total=$(jq "[$ctx | .[] | select(.isRequired==true)] | length" <<<"$pr")
        ci_fail=$(jq "[$ctx | .[] | select(.isRequired==true) | select(
            (.__typename==\"CheckRun\"      and .status==\"COMPLETED\" and ((.conclusion // \"\") | IN(\"FAILURE\",\"TIMED_OUT\",\"CANCELLED\",\"ACTION_REQUIRED\",\"STARTUP_FAILURE\"))) or
            (.__typename==\"StatusContext\" and ((.state // \"\") | IN(\"FAILURE\",\"ERROR\")))
        )] | length" <<<"$pr")
        ci_pend=$(jq "[$ctx | .[] | select(.isRequired==true) | select(
            (.__typename==\"CheckRun\"      and .status!=\"COMPLETED\") or
            (.__typename==\"StatusContext\" and ((.state // \"\") | IN(\"PENDING\",\"EXPECTED\")))
        )] | length" <<<"$pr")

        # CodeRabbit — three-bucket (NONE / STALE / latest-on-head)
        local cr_state
        cr_state=$(jq -r --arg sha "$head_sha" '
            ([.reviews.nodes[]
              | select(.author.login=="coderabbitai" or .author.login=="coderabbitai[bot]")]) as $all
            | if ($all | length) == 0 then "NONE"
              else (([$all[] | select(.commit.oid==$sha)]) as $current
                   | if ($current | length) == 0 then "STALE"
                     else ($current | sort_by(.submittedAt) | .[-1].state)
                     end)
              end' <<<"$pr")
        local cr_open
        cr_open=$(jq '[.reviewThreads.nodes[]
            | select(.isResolved==false and .isOutdated==false
                     and any(.comments.nodes[];
                             .author.login=="coderabbitai" or .author.login=="coderabbitai[bot]"))] | length' <<<"$pr")

        # User comments — typename bot filter, login case-insensitive
        local user
        user=$(jq --arg self "$ORCH_USER" '
            ([.comments.nodes[]
              | select(.author.__typename != "Bot"
                       and ((.author.login // "") | ascii_downcase) != ($self | ascii_downcase))] | length) +
            ([.reviewThreads.nodes[]
              | select(.isResolved==false and .isOutdated==false
                       and any(.comments.nodes[];
                               .author.__typename != "Bot"
                               and ((.author.login // "") | ascii_downcase) != ($self | ascii_downcase)))] | length)
        ' <<<"$pr")

        # reviewDecision
        local review_decision
        review_decision=$(jq -r '.reviewDecision // "NONE"' <<<"$pr")
        local review_pass=false
        case "$review_decision" in
            APPROVED|NONE) review_pass=true ;;
        esac

        local cr_pass=false
        case "$cr_state" in
            APPROVED|COMMENTED|NONE) cr_pass=true ;;
        esac

        printf 'Poll %d/%d — CI: %d/%d pass (%d fail, %d pending) | CodeRabbit: %s (%d open) | User: %d | reviewDecision: %s\n' \
            $((p+1)) "$MAX_POLLS" $((ci_total - ci_fail - ci_pend)) "$ci_total" "$ci_fail" "$ci_pend" \
            "$cr_state" "$cr_open" "$user" "$review_decision"

        if [ "$ci_fail" -eq 0 ] && [ "$ci_pend" -eq 0 ] && \
           [ "$cr_pass" = true ] && [ "$cr_open" -eq 0 ] && \
           [ "$user" -eq 0 ] && [ "$review_pass" = true ]; then
            echo "GATES_PASSED"
            return 0
        fi

        local elapsed=$(( $(date +%s) - start ))
        if [ "$elapsed" -ge "$TIMEOUT_SECONDS" ]; then
            echo "GATES_TIMEOUT"
            return 2
        fi

        if [ "$p" -lt $((MAX_POLLS-1)) ]; then
            sleep "$POLL_INTERVAL"
        fi
    done

    return 1
}

# ----------------------------------------------------------------------------
# gh_pr_ready_idempotent <pr_number>
# ----------------------------------------------------------------------------
gh_pr_ready_idempotent() {
    local prNumber="${1:?gh_pr_ready_idempotent: pr_number required}"
    local out
    if ! out=$(gh pr ready "$prNumber" 2>&1); then
        case "$out" in
            *"not in draft state"*|*"already marked ready"*)
                # Idempotent — already non-draft.
                return 0
                ;;
            *)
                echo "$out" >&2
                return 6
                ;;
        esac
    fi
}

# ----------------------------------------------------------------------------
# CLI entry point — only when invoked directly (not sourced).
# ----------------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    poll_merge_gates "$@"
fi
