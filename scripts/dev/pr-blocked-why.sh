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

# Source merge-gates.sh ONLY for the single-source blocking-scope constant, so
# a non-required RED check is classified identically to the merge gate
# (block-on-any-red). Side-effect-free when sourced. The fallback mirrors the
# sourced value ("." = every name) — a stale curated-list fallback here once
# made this diagnostic silently disagree with the poller when sourcing failed.
# shellcheck source=agents/scripts/core/merge-gates.sh
source "$REPO_ROOT/agents/scripts/core/merge-gates.sh" 2>/dev/null || true
ALLOW_RE="${MERGE_GATES_BLOCK_ALLOWLIST_RE:-.}"

# GOTCHA (c): `gh api <path>` REST paths must be SLASH-RELATIVE (`repos/...`,
# `user`), NOT absolute (`/repos/...`). A leading slash makes gh build
# `https://api.github.com//repos/...` (double slash) → an HTTP 404 that reads
# like a missing resource. This wrapper strips any accidental leading `/` so a
# caller can pass either form safely. Used for every REST (non-graphql) call.
gh_rest() {
    local path="${1#/}"; shift
    gh api "$path" "$@"
}

# urlencode_segment <s> — percent-encode a single REST path SEGMENT so a value
# containing `/` (a branch name like `claude/foo/bar`) does not split the path
# into extra segments. Encodes every char outside the unreserved set
# (A-Z a-z 0-9 - . _ ~) — crucially `/` → %2F — so the encoded value stays one
# path segment. Pure bash (no jq/python dependency); used for branch names that
# interpolate into a REST URL path.
urlencode_segment() {
    local s="$1" out="" i c
    for (( i = 0; i < ${#s}; i++ )); do
        c="${s:i:1}"
        case "$c" in
            [a-zA-Z0-9.~_-]) out+="$c" ;;
            *) printf -v c '%%%02X' "'$c"; out+="$c" ;;
        esac
    done
    printf '%s' "$out"
}

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
# read_conversation_resolution_required <owner> <repo> <base_branch> — echo
# "true" / "false" / "unknown" for the base branch's
# required_conversation_resolution branch-protection setting. "unknown" on any
# read failure (no gh, fork, API error, missing field) — the classifier treats
# unknown as fail-safe (still surfaces unresolved threads as the likely cause).
# Override with PR_BLOCKED_WHY_CONV_RES_REQUIRED (true/false/unknown) for tests.
# Tries the rulesets-aware classic branch-protection endpoint first, then the
# GraphQL branchProtectionRule as a fallback.
# ----------------------------------------------------------------------------
read_conversation_resolution_required() {
    if [ -n "${PR_BLOCKED_WHY_CONV_RES_REQUIRED+x}" ]; then
        printf '%s' "${PR_BLOCKED_WHY_CONV_RES_REQUIRED:-unknown}"
        return 0
    fi
    local owner="$1" repo="$2" base="$3"
    command -v gh >/dev/null 2>&1 || { printf 'unknown'; return 0; }
    [ -n "$base" ] || { printf 'unknown'; return 0; }
    local val base_enc
    # URL-encode the branch name as a single path SEGMENT before interpolating it
    # into the REST path: a name with `/` (e.g. `claude/foo/bar`) would otherwise
    # split into extra segments and 404 the wrong URL (`.../branches/claude/foo/
    # bar/protection`). owner/repo come from `gh repo view` (no slashes), so only
    # $base needs encoding.
    base_enc=$(urlencode_segment "$base")
    # Classic branch-protection REST: required_conversation_resolution.enabled.
    val=$(gh_rest "repos/$owner/$repo/branches/$base_enc/protection" \
              --jq '.required_conversation_resolution.enabled' 2>/dev/null) || val=""
    case "$val" in
        true)  printf 'true';  return 0 ;;
        false) printf 'false'; return 0 ;;
    esac
    # Fallback: GraphQL branchProtectionRule.requiresConversationResolution.
    # $base is a typed GraphQL variable (-f base=) so the query is parametrized;
    # the rule match is done with a separate standalone-jq pass keyed on a --arg
    # (NOT shell-interpolated into the jq program) so a branch name containing a
    # quote / backslash / slash cannot break the filter. `gh api --jq` does not
    # forward --arg, so the filtering jq runs as its own pipeline stage.
    val=$(gh api graphql -f owner="$owner" -f repo="$repo" -f base="$base" \
              -f query='query($owner:String!,$repo:String!,$base:String!){repository(owner:$owner,name:$repo){branchProtectionRules(first:50){nodes{pattern requiresConversationResolution}}}}' \
              --jq '.data.repository.branchProtectionRules.nodes' 2>/dev/null \
            | jq -r --arg base "$base" '(. // [])[] | select(.pattern == $base) | .requiresConversationResolution' 2>/dev/null) || val=""
    case "$val" in
        true)  printf 'true' ;;
        false) printf 'false' ;;
        *)     printf 'unknown' ;;
    esac
}

# ----------------------------------------------------------------------------
# classify_blockers <pr_json> <req-newline-list> <orch_user> — the PURE core,
# fully testable with no `gh`. Reads the pullRequest object and emits a
# human-readable, multi-line blocker report on stdout. Exit 0 always (a parse
# failure prints a fail-closed "could not evaluate" line, never a silent pass).
#
# Classes (each printed when present):
#   CONVERSATION — branch protection requires conversation resolution AND the PR
#                  has unresolved review threads. This is a COMMON silent BLOCKED
#                  cause: every CI check is green, reviewDecision is APPROVED, yet
#                  GitHub holds the merge purely because require_conversation_-
#                  resolution is on and one thread is open. Promoted to a TOP-LEVEL
#                  class (not just a THREADS sub-detail) because it is the precise
#                  reason a green-looking PR will not merge — resolve the threads
#                  (or, for a confirmed CR false positive, the cr-out-of-band +
#                  cr-disposition + resolveReviewThread flow in merge-gates.md).
#   THREADS  — unresolved, non-outdated review threads (path + first author)
#   CHECKS   — required-or-allow-listed checks that are FAILING or PENDING
#   ABSENT   — required contexts absent from the head rollup (never ran)
#   REVIEW   — reviewDecision is not APPROVED/null (a required review is owed)
#   STATE    — mergeStateStatus / state (context line, always printed)
#
# require_conversation_resolution ground truth: read from the branch-protection
# rule on the PR's base branch and threaded in as the $convResReq jq arg
# ("true"/"false"/"unknown"). When the live rule cannot be read (no gh / fork /
# API error) it is "unknown" → the classifier still reports unresolved threads as
# a CONVERSATION blocker (fail-safe: an unresolved thread on a BLOCKED PR is the
# likely cause regardless), just without asserting the rule as confirmed-on.
# ----------------------------------------------------------------------------
classify_blockers() {
    local pr_json="$1" req_list="$2" orch="$3" conv_res_req="${4:-unknown}"
    local req_json='[]'
    if [ -n "$req_list" ]; then
        req_json=$(printf '%s\n' "$req_list" | jq -R . | jq -sc 'map(select(length > 0))') || req_json='[]'
    fi

    local out
    if ! out=$(printf '%s' "$pr_json" | jq -r \
        --argjson req "$req_json" \
        --arg allow "$ALLOW_RE" \
        --arg orch "$orch" \
        --arg convResReq "$conv_res_req" '
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
          (if ($threads | length) > 0
             then "CONVERSATION — \($threads | length) unresolved review thread(s)"
                  + (if $convResReq == "true"
                       then " AND branch protection requires conversation resolution: this BLOCKS merge even with every check green. Resolve the thread(s) (or, for a confirmed CR false positive, use the cr-out-of-band + cr-disposition + resolveReviewThread flow in merge-gates.md)."
                     elif $convResReq == "unknown"
                       then " present; require_conversation_resolution could not be confirmed (no gh / fork / API error) but an unresolved thread is the likely BLOCKED cause. Resolve the thread(s) or confirm the rule."
                     else " present; require_conversation_resolution is OFF on the base branch, so these do NOT by themselves block merge — but a human/bot still expects a reply." end)
             else empty end),
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
      baseRefName
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

    # CASE 8 — GOTCHA (c): gh_rest strips a leading slash on a REST path so
    # `gh api` never builds a double-slash URL. Stub `gh` to echo the path it
    # received; assert both "/user" and "user" resolve to the same slash-relative
    # form. (Verifies the Windows/Git-Bash REST-path convention the helper bakes in.)
    local gh_stub_dir captured
    gh_stub_dir=$(mktemp -d "${TMPDIR:-/tmp}/pr-blocked-why-ghstub.XXXXXX")
    cat > "$gh_stub_dir/gh" <<'STUB'
#!/usr/bin/env bash
# Stub gh: print the api path argument so the caller can assert slash-stripping.
[ "$1" = "api" ] && { echo "PATH=$2"; exit 0; }
exit 0
STUB
    chmod +x "$gh_stub_dir/gh"
    captured=$(PATH="$gh_stub_dir:$PATH" gh_rest /user --jq .login 2>/dev/null)
    local captured2
    captured2=$(PATH="$gh_stub_dir:$PATH" gh_rest user --jq .login 2>/dev/null)
    rm -rf "$gh_stub_dir"
    if [ "$captured" = "PATH=user" ] && [ "$captured2" = "PATH=user" ]; then
        echo "selftest CASE8 PASS — gh_rest strips a leading slash on REST paths"
    else
        echo "selftest CASE8 FAIL — leading slash should be stripped (got: '$captured' / '$captured2')" >&2
        fails=$((fails + 1))
    fi

    # CASE 9 — CONVERSATION class: unresolved threads + require_conversation_-
    # resolution ON is reported as a top-level CONVERSATION blocker that names the
    # rule (the common silent-BLOCKED cause with every check green + APPROVED).
    local j9
    j9='{"state":"OPEN","mergeStateStatus":"BLOCKED","reviewDecision":"APPROVED","baseRefName":"develop",
      "commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[
        {"__typename":"StatusContext","context":"Windows + MSVC","state":"SUCCESS","isRequired":true}]}}}}]},
      "reviewThreads":{"nodes":[
        {"isResolved":false,"isOutdated":false,"path":"src/foo.cpp",
         "comments":{"nodes":[{"author":{"login":"coderabbitai","__typename":"Bot"}}]}}]}}'
    out=$(classify_blockers "$j9" "Windows + MSVC" orch true)
    if printf '%s' "$out" | grep -q 'CONVERSATION' \
       && printf '%s' "$out" | grep -q 'requires conversation resolution'; then
        echo "selftest CASE9 PASS — CONVERSATION blocker named when rule is ON"
    else
        echo "selftest CASE9 FAIL — should name the CONVERSATION blocker (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 10 — require_conversation_resolution UNKNOWN (rule unreadable): still
    # surfaces a CONVERSATION line (fail-safe) but does NOT assert the rule is on.
    out=$(classify_blockers "$j9" "Windows + MSVC" orch unknown)
    if printf '%s' "$out" | grep -q 'CONVERSATION' \
       && printf '%s' "$out" | grep -q 'could not be confirmed' \
       && ! printf '%s' "$out" | grep -q 'requires conversation resolution'; then
        echo "selftest CASE10 PASS — CONVERSATION fail-safe when rule unknown"
    else
        echo "selftest CASE10 FAIL — unknown rule should fail-safe (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 11 — require_conversation_resolution OFF: unresolved threads do NOT by
    # themselves block; the CONVERSATION line says so (no false BLOCKED claim).
    out=$(classify_blockers "$j9" "Windows + MSVC" orch false)
    if printf '%s' "$out" | grep -q 'CONVERSATION' \
       && printf '%s' "$out" | grep -q 'is OFF'; then
        echo "selftest CASE11 PASS — CONVERSATION notes rule OFF (non-blocking)"
    else
        echo "selftest CASE11 FAIL — OFF rule should not claim a block (got: '$out')" >&2
        fails=$((fails + 1))
    fi

    # CASE 12 — urlencode_segment percent-encodes `/` (and reserved chars) so a
    # branch name like `claude/foo/bar` stays a SINGLE REST path segment. Without
    # this, `repos/o/r/branches/claude/foo/bar/protection` 404s the wrong URL.
    local enc
    enc=$(urlencode_segment "claude/foo/bar")
    if [ "$enc" = "claude%2Ffoo%2Fbar" ]; then
        echo "selftest CASE12 PASS — urlencode_segment encodes slashes to %2F"
    else
        echo "selftest CASE12 FAIL — slashes should encode to %2F (got: '$enc')" >&2
        fails=$((fails + 1))
    fi

    # CASE 13 — end-to-end: read_conversation_resolution_required builds the REST
    # path with the ENCODED branch (single segment). Stub gh to LOG every REST path
    # it received to a file (so both the classic + graphql invocations are visible);
    # assert the classic-protection path carried the slashes as %2F.
    local gh_stub_dir2 stub_log captured_path
    gh_stub_dir2=$(mktemp -d "${TMPDIR:-/tmp}/pr-blocked-why-ghstub2.XXXXXX")
    stub_log="$gh_stub_dir2/calls.log"
    cat > "$gh_stub_dir2/gh" <<STUB
#!/usr/bin/env bash
# Stub gh: log the REST path so the caller can assert branch URL-encoding.
[ "\$1" = "api" ] && echo "REST_PATH=\$2" >> "$stub_log"
exit 0
STUB
    chmod +x "$gh_stub_dir2/gh"
    ( unset PR_BLOCKED_WHY_CONV_RES_REQUIRED
      PATH="$gh_stub_dir2:$PATH" read_conversation_resolution_required owner repo "claude/foo/bar" ) >/dev/null 2>&1
    captured_path=$(cat "$stub_log" 2>/dev/null)
    rm -rf "$gh_stub_dir2"
    if printf '%s' "$captured_path" | grep -q 'branches/claude%2Ffoo%2Fbar/protection'; then
        echo "selftest CASE13 PASS — slashed branch URL-encoded into the REST path"
    else
        echo "selftest CASE13 FAIL — branch should be %2F-encoded in REST path (got: '$captured_path')" >&2
        fails=$((fails + 1))
    fi

    if [ "$fails" -eq 0 ]; then
        echo "PASS — pr-blocked-why --selftest (13/13)"
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
        [ -n "$orch" ] || orch=$(gh_rest user --jq .login 2>/dev/null) || orch=""
        # GOTCHA (a): derive owner/repo from `gh repo view --json nameWithOwner`,
        # NEVER a hardcoded slug — a worktree of a fork (or a renamed repo) would
        # otherwise query the wrong PR. nameWithOwner is "<owner>/<repo>".
        local nwo owner repo
        nwo=$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null) || nwo=""
        owner="${nwo%%/*}"; repo="${nwo##*/}"
        if [ -z "$owner" ] || [ -z "$repo" ]; then
            echo "pr-blocked-why: cannot resolve owner/repo (run inside the repo)." >&2
            exit 2
        fi
        # GOTCHA (b): pass the GraphQL document via `-F query=@<file>` (a typed
        # field that gh reads FROM the file), not inline `-f query="$GRAPHQL"`.
        # Under Git-Bash on Windows a multi-line inline string is fragile —
        # CRLF / quoting / the leading newline in $GRAPHQL can mangle the body
        # before gh sees it. A heredoc to a tmpfile + `-F …=@file` sidesteps all
        # shell quoting: gh slurps the raw bytes. (Distinct from `-f query=@file`,
        # which sends the LITERAL "@file" string — only `-F` dereferences `@`.)
        local query_file
        query_file=$(mktemp "${TMPDIR:-/tmp}/pr-blocked-why-query.XXXXXX") || {
            echo "pr-blocked-why: mktemp failed" >&2; exit 2; }
        # shellcheck disable=SC2064  # expand query_file now for the EXIT trap
        trap "rm -f '$query_file'" EXIT
        cat > "$query_file" <<EOF
$GRAPHQL
EOF
        # GOTCHA (d): read the response as UTF-8. `gh api --jq` uses gh's bundled
        # jq (UTF-8 by construction), and we force a UTF-8 locale so the
        # downstream standalone-`jq` parse in classify_blockers never mojibakes a
        # multibyte path / author / em-dash check name under a cp1252 default.
        if ! pr_json=$(LC_ALL="${LC_ALL:-C.UTF-8}" gh api graphql \
                          -f owner="$owner" -f repo="$repo" -F pr="$pr" \
                          -F query=@"$query_file" \
                          --jq '.data.repository.pullRequest' 2>&1); then
            echo "pr-blocked-why: gh api graphql failed: $pr_json" >&2
            rm -f "$query_file"; trap - EXIT
            exit 3
        fi
        rm -f "$query_file"; trap - EXIT
    fi

    echo "PR #$pr — blocker analysis:"
    local req_list
    req_list=$(read_required_contexts)
    # require_conversation_resolution ground truth for the CONVERSATION class —
    # read from the PR's base-branch protection rule. "unknown" when unreadable
    # (stub-JSON path with no base / no gh / fork) → classifier fail-safe.
    local base conv_res
    base=$(printf '%s' "$pr_json" | jq -r '.baseRefName // ""' 2>/dev/null) || base=""
    if [ -n "${PR_BLOCKED_WHY_STUB_JSON:-}" ]; then
        conv_res="${PR_BLOCKED_WHY_CONV_RES_REQUIRED:-unknown}"
    else
        conv_res=$(read_conversation_resolution_required "$owner" "$repo" "$base")
    fi
    classify_blockers "$pr_json" "$req_list" "$orch" "$conv_res"
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
