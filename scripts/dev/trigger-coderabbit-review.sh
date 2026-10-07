#!/usr/bin/env bash
# trigger-coderabbit-review.sh — the missing ship-loop step for OSS <10-star repos.
#
# WHY: CodeRabbit auto-reviews only repos with ≥10 stars. Below that it posts
# "Review available on request" / "manual review required for this OSS
# repository" and waits. Bot-authored `@coderabbitai review` (GITHUB_TOKEN /
# github-actions[bot] / GitHub App installation tokens) is IGNORED — only a
# human-authored comment triggers review (process 2026-08-30).
#
# This script is the sanctioned move:
#   1. Detect the OSS manual-trigger state on the PR.
#   2. If `gh` is authenticated as a USER (not a bot/app), post
#      `@coderabbitai review` once and exit 0.
#   3. If the token cannot speak as a human, print the exact AskUserQuestion
#      options (human comment OR cr-out-of-band + disposition) and exit 2.
#
# Usage:
#   bash scripts/dev/trigger-coderabbit-review.sh <pr>
#   bash scripts/dev/trigger-coderabbit-review.sh --selftest
#
# Exit:
#   0 — trigger posted (or already present from a human), OR selftest OK
#   1 — CR already reviewed this head (nothing to do)
#   2 — cannot post as a human; operator action required (printed)
#   3 — usage / gh / API error
#
# selftest: asserts-failure
set -euo pipefail

SELFTEST=false
PR=""
case "${1:-}" in
    --selftest) SELFTEST=true ;;
    ""|-h|--help)
        echo "usage: $0 <pr> | --selftest" >&2
        exit 3
        ;;
    *) PR="$1" ;;
esac

# classify_author_kind <login> <typename> — user | bot | unknown
classify_author_kind() {
    local login="${1:-}" typename="${2:-}"
    case "$typename" in
        Bot) printf 'bot'; return ;;
    esac
    case "$login" in
        "" ) printf 'unknown' ;;
        *\[bot\]|github-actions|github-actions\[bot\]|dependabot*|cursor) printf 'bot' ;;
        *) printf 'user' ;;
    esac
}

# can_post_as_human — 0 if gh auth looks like a user account, 1 otherwise.
# Installation tokens (ghs_*) and bot logins cannot trigger CodeRabbit.
can_post_as_human() {
    local login="" token_kind=""
    login="$(gh api user --jq .login 2>/dev/null || true)"
    if [ -z "$login" ]; then
        return 1
    fi
    case "$(classify_author_kind "$login" "")" in
        bot) return 1 ;;
    esac
    # gh auth status prints Token: gho_/ghp_ (user) vs ghs_ (installation).
    token_kind="$(gh auth status 2>&1 | grep -oE 'Token:[[:space:]]*ghs_|Token:[[:space:]]*gho_|Token:[[:space:]]*ghp_|Token:[[:space:]]*github_pat_' | head -1 || true)"
    case "$token_kind" in
        *ghs_*) return 1 ;;
    esac
    return 0
}

print_escalate() {
    local pr="$1"
    cat <<EOF
ESCALATE: CodeRabbit needs a HUMAN trigger on PR #${pr} (repo <10 stars).

Bot/app \`@coderabbitai review\` comments are ignored. Pick one:

  (1) Human comment on the PR:
        @coderabbitai review
      Then wait for the CR finding gate to re-run.

  (2) Waive CR for this PR (auto_merge / silent-CR path):
        gh label create cr-disposition:cr-auto-review-disabled --force 2>/dev/null || true
        gh pr edit ${pr} --add-label cr-out-of-band --add-label cr-disposition:cr-auto-review-disabled
      The cr-finding-gate workflow re-runs on \`labeled\` and posts success.

  (3) Durable fix (human account action, not a gate): star the repo to ≥10
      or enable a CodeRabbit plan that auto-reviews.

Helper: bash scripts/dev/trigger-coderabbit-review.sh ${pr}
Playbook: agent-layer/docs/agent-rules/merge-gates.md § CodeRabbit OSS manual-trigger
EOF
}

# count_head_asks <graphql-json> <head-sha> [<anchor>] — human `@coderabbitai review` asks
# that belong to THIS head: carrying this script's per-head marker, or posted after the
# head's anchor. The anchor is CodeRabbit's FIRST status on the head (CR posts one on every
# head, right after the push), else the commit date. It must not be CR's latest status:
# that moves every time CR updates it — its rate-limit reply to the ask itself, say — and an
# ask on this head would then stop counting and be posted again. An ask from an earlier
# head does not count, or a later head would never get the trigger it needs.
count_head_asks() {
    printf '%s' "$1" | jq -r --arg h "$2" --arg first "${3:-}" '
      .data.repository.pullRequest as $pr
      | ($pr.commits.nodes[0].commit // {}) as $c
      | (if $first != "" then $first else ($c.committedDate // "") end) as $anchor
      | [ $pr.comments.nodes[]?
          | select(.author.__typename != "Bot"
                   and ((.author.login // "") | test("\\[bot\\]$|github-actions|^cursor$") | not)
                   and ((.body // "") | test("@coderabbitai[[:space:]]+(full[[:space:]]+)?review"; "i")))
          | select(((.body // "") | contains("cr-human-first-review-nudge:" + $h))
                   or ($anchor != "" and (.createdAt // "") > $anchor)) ]
      | length'
}

if [ "$SELFTEST" = true ]; then
    [ "$(classify_author_kind 'alice' 'User')" = user ]
    [ "$(classify_author_kind 'github-actions[bot]' 'Bot')" = bot ]
    [ "$(classify_author_kind 'cursor' '')" = bot ]
    [ "$(classify_author_kind 'coderabbitai[bot]' 'Bot')" = bot ]
    if command -v jq >/dev/null 2>&1; then
        # CR's first status on this head is 10:00; its latest (rollup) is 10:10, after it
        # answered an ask. An ask from an earlier head (before 10:00) does not count; one
        # after 10:00 does, even though CR updated its status after it, and so does one
        # carrying this head's marker.
        st_json() {
            printf '{"data":{"repository":{"pullRequest":{"headRefOid":"bbb","commits":{"nodes":[{"commit":{"committedDate":"2026-10-01T09:00:00Z","statusCheckRollup":{"contexts":{"nodes":[{"__typename":"StatusContext","context":"CodeRabbit","createdAt":"2026-10-01T10:10:00Z"}]}}}}]},"comments":{"nodes":[%s]}}}}}' "$1"
        }
        first=2026-10-01T10:00:00Z
        old='{"author":{"login":"alice","__typename":"User"},"body":"@coderabbitai review","createdAt":"2026-10-01T08:00:00Z"}'
        new='{"author":{"login":"alice","__typename":"User"},"body":"@coderabbitai review","createdAt":"2026-10-01T10:05:00Z"}'
        marked='{"author":{"login":"alice","__typename":"User"},"body":"@coderabbitai review <!-- cr-human-first-review-nudge:bbb -->","createdAt":"2026-10-01T08:00:00Z"}'
        bot='{"author":{"login":"github-actions[bot]","__typename":"Bot"},"body":"@coderabbitai review","createdAt":"2026-10-01T10:05:00Z"}'
        early='{"author":{"login":"alice","__typename":"User"},"body":"@coderabbitai review","createdAt":"2026-10-01T09:30:00Z"}'
        [ "$(count_head_asks "$(st_json "$old")" bbb "$first")" = 0 ]
        [ "$(count_head_asks "$(st_json "$new")" bbb "$first")" = 1 ]
        [ "$(count_head_asks "$(st_json "$marked")" bbb "$first")" = 1 ]
        [ "$(count_head_asks "$(st_json "$bot")" bbb "$first")" = 0 ]
        # No CR status on the head yet: the commit date (09:00) is the anchor.
        [ "$(count_head_asks "$(st_json "$early")" bbb "")" = 1 ]
        [ "$(count_head_asks "$(st_json "$old")" bbb "")" = 0 ]
    fi
    echo "selftest: trigger-coderabbit-review OK"
    exit 0
fi

command -v gh >/dev/null 2>&1 || { echo "trigger-coderabbit-review: gh required" >&2; exit 3; }
command -v jq >/dev/null 2>&1 || { echo "trigger-coderabbit-review: jq required" >&2; exit 3; }

# Resolve owner/repo from the checkout.
owner_repo="$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || true)"
if [ -z "$owner_repo" ]; then
    echo "trigger-coderabbit-review: cannot resolve owner/repo (run inside the repo)" >&2
    exit 3
fi
owner="${owner_repo%/*}"
repo="${owner_repo#*/}"

# Head + existing reviews/comments.
data="$(gh api graphql -f query='
query($o:String!,$r:String!,$n:Int!) {
  repository(owner:$o,name:$r) {
    pullRequest(number:$n) {
      headRefOid
      reviews(last:40) { nodes { author { login __typename } commit { oid } } }
      comments(last:40) { nodes { author { login __typename } body createdAt } }
      commits(last:1) { nodes { commit { committedDate statusCheckRollup { contexts(first:100) {
        nodes { __typename ... on StatusContext { context createdAt } } } } } } }
    }
  }
}' -F o="$owner" -F r="$repo" -F n="$PR" 2>/dev/null)" || {
    echo "trigger-coderabbit-review: GraphQL failed for PR #$PR" >&2
    exit 3
}

head="$(printf '%s' "$data" | jq -r '.data.repository.pullRequest.headRefOid // empty')"
[ -n "$head" ] || { echo "trigger-coderabbit-review: PR #$PR not found" >&2; exit 3; }

# Already has a CR review on this exact head → nothing to trigger.
on_head="$(printf '%s' "$data" | jq -r --arg h "$head" '
  [.data.repository.pullRequest.reviews.nodes[]?
   | select((.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")
            and (.commit.oid // "") == $h)] | length')"
if [ "${on_head:-0}" -gt 0 ]; then
    echo "INFO: CodeRabbit already reviewed head ${head:0:8} on PR #$PR — no trigger needed."
    exit 1
fi

# A human already asked for THIS head → do not double-post; CR may still be working.
# CodeRabbit's first status on the head anchors which asks belong to it (see count_head_asks).
cr_first_status="$(gh api --paginate "repos/$owner/$repo/commits/$head/statuses?per_page=100" \
    --jq '.[] | select(.context == "CodeRabbit") | .created_at' 2>/dev/null | sort | sed -n 1p)" \
    || cr_first_status=""
human_nudge="$(count_head_asks "$data" "$head" "$cr_first_status")"
if [ "${human_nudge:-0}" -gt 0 ]; then
    echo "INFO: human @coderabbitai review already on PR #$PR for head ${head:0:8} — waiting for CodeRabbit."
    exit 0
fi

if ! can_post_as_human; then
    print_escalate "$PR"
    exit 2
fi

body='@coderabbitai review

<!-- cr-human-first-review-nudge:'"$head"' -->
_Posted by scripts/dev/trigger-coderabbit-review.sh — human-credentialed trigger for OSS <10-star repos (bot nudges are ignored)._'

if gh pr comment "$PR" --repo "$owner/$repo" --body "$body" >/dev/null; then
    echo "INFO: posted human @coderabbitai review on PR #$PR (head ${head:0:8})."
    echo "INFO: wait for CodeRabbit + the CR finding gate to re-run."
    exit 0
fi

echo "WARN: gh pr comment failed." >&2
print_escalate "$PR"
exit 2
