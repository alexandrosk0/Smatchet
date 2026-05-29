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
# Rollup dedup: required CheckRuns with the same `.name` are deduped to the
# entry with the latest `.startedAt` so stale FAILUREs from rerun jobs don't
# falsely block. StatusContexts are deduped by `.context` (GitHub overwrites).
#
# Per-PR label overrides (AGENTS.md § Merge gates § Per-PR overrides):
#   tests-out-of-band → downgrades `Test-delta gate` FAIL → WARN
#   perf-out-of-band  → downgrades `Perf PR-fast (...)` FAIL → WARN
# Downgraded failures are logged on stderr but do NOT contribute to ci_fail.
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
#   MERGE_GATES_FLIP_READY       — when "true", flip PR ready-for-review at
#                                  poll start (authorized-merge callers only)
#   MERGE_GATES_CR_GRACE_POLLS   — CR review grace window (default 10 polls)
#   MERGE_GATES_CR_INSTALLED     — override CR-installed auto-detection
#   MERGE_GATES_STALE_REREVIEW_POLLS — consecutive STALE polls on same HEAD
#                                  before auto-posting `@coderabbitai review`
#                                  (default 5; 0 disables)
#
# Manual CR re-review trigger: post `@coderabbitai review` as a PR comment
# (`gh pr comment <pr> --body "@coderabbitai review"`) when CR's review is
# STALE on a new HEAD and isn't auto-firing.
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

set -uo pipefail

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
    # Deps preflight scoped to function call — file is documented sourceable
    # (see header § Usage). Top-level `exit` would kill the caller's shell.
    command -v gh >/dev/null 2>&1 || { echo "gh required" >&2; return 2; }
    # No standalone `jq` needed — the poll parses the GraphQL response via
    # gh's bundled jq engine (`gh api --jq`). gh is the only hard dep.

    # SKIP_MERGE_GATES=true at session init bypasses all gates. Documented in
    # AGENTS.md § Merge gates and docs/agent-rules/merge-gates.md § Override.
    # Until this guard landed (PR for C1 in docs/evaluation/agentic-infrastructure-2026-05-23.md),
    # the override was a pure documentation contract — every caller was trusted
    # to gate the call itself. A miswired delegated invocation could quietly
    # poll regardless. Read FIRST (before ORCH_USER + every other prereq) so
    # the bypass is unconditional — a skipped gate doesn't need ORCH_USER to
    # be set, doesn't need the query file to exist, doesn't need anything.
    if [ "${SKIP_MERGE_GATES:-}" = "true" ]; then
        echo "GATES_SKIPPED (SKIP_MERGE_GATES=true)"
        return 0
    fi

    if [ -z "${ORCH_USER:-}" ]; then
        echo "poll_merge_gates: ORCH_USER not set (run: ORCH_USER=\$(gh api user --jq .login))" >&2
        return 3
    fi

    # MERGE_GATES_FLIP_READY=true flips the PR ready-for-review BEFORE polling starts.
    # Authorized-merge callers (orchestrator + smatchet-merge-watcher) opt in so that
    # CodeRabbit's auto_review.drafts:false config doesn't bypass review on draft PRs.
    # Without this, CR's placeholder StatusContext SUCCESS could let a draft PR pass
    # through the grace window without any real review activity (C4 draft-PR bypass).
    # Plain poll-only callers (status checks, dry-runs) leave this unset; the gate's
    # CR-installed grace window still blocks NONE for installed repos.
    if [ "${MERGE_GATES_FLIP_READY:-}" = "true" ]; then
        gh_pr_ready_idempotent "$prNumber" || \
            echo "WARN: gh_pr_ready_idempotent returned non-zero; PR may still be draft." >&2
    fi

    local POLL_INTERVAL="${MERGE_GATES_POLL_INTERVAL:-60}"
    local MAX_POLLS="${MERGE_GATES_MAX_POLLS:-60}"
    local TIMEOUT_SECONDS="${MERGE_GATES_TIMEOUT_SECONDS:-3600}"
    local QUERY_FILE="${MERGE_GATES_QUERY_FILE:-$DEFAULT_QUERY_FILE}"
    # When CR state is STALE_WITH_FINDINGS / STALE_UNKNOWN on the same HEAD for
    # this many consecutive polls, post `@coderabbitai review` once per HEAD to
    # nudge CR into re-reviewing. Default 5 polls (~5 min at default interval).
    # Set to 0 to disable the auto-trigger.
    local STALE_REREVIEW_POLLS="${MERGE_GATES_STALE_REREVIEW_POLLS:-5}"
    if ! [[ "$STALE_REREVIEW_POLLS" =~ ^[0-9]+$ ]]; then
        echo "poll_merge_gates: MERGE_GATES_STALE_REREVIEW_POLLS must be a non-negative integer (got: $STALE_REREVIEW_POLLS)" >&2
        return 3
    fi
    # Number of consecutive polls the gate will wait for CodeRabbit when the repo has
    # `.coderabbit.yaml` checked in (= CR is installed for this repo). After this many
    # polls without a review or a `CodeRabbit` SUCCESS StatusContext, NONE falls back
    # to pass with a logged warning so the loop is never wedged by a stuck integration.
    local CR_GRACE_POLLS="${MERGE_GATES_CR_GRACE_POLLS:-10}"

    if [ ! -f "$QUERY_FILE" ]; then
        echo "poll_merge_gates: query file not found: $QUERY_FILE" >&2
        return 3
    fi

    # Detect whether CodeRabbit is installed for this repo by probing for a checked-in
    # `.coderabbit.yaml` (or `.coderabbit.yml`). The `auto_review.drafts: false` default
    # plus CR's eventual-consistency means a freshly-opened PR can race the poller —
    # NONE on Poll 1 is a race, not "CR not installed". Override via env if needed.
    #
    # H12: separate 404 (file truly absent → cr_installed=false) from other
    # errors (auth, network, transient — fail safe, cr_installed=true). The
    # previous probe treated any non-zero `gh api` exit as "absent", which
    # silently disabled the CR gate on auth failures or transient network
    # blips. Fail-safe direction = assume installed so the gate blocks on
    # unknown CR state instead of waving through.
    local cr_installed
    if [ -n "${MERGE_GATES_CR_INSTALLED:-}" ]; then
        cr_installed="$MERGE_GATES_CR_INSTALLED"
    else
        local yaml_err="" yaml_rc=0 yml_err="" yml_rc=0
        yaml_err=$(gh api "repos/$owner/$repo/contents/.coderabbit.yaml" 2>&1 >/dev/null) || yaml_rc=$?
        if [ "$yaml_rc" -eq 0 ]; then
            cr_installed=true
        elif echo "$yaml_err" | grep -q "HTTP 404"; then
            # .yaml confirmed 404; try .yml
            yml_err=$(gh api "repos/$owner/$repo/contents/.coderabbit.yml" 2>&1 >/dev/null) || yml_rc=$?
            if [ "$yml_rc" -eq 0 ]; then
                cr_installed=true
            elif echo "$yml_err" | grep -q "HTTP 404"; then
                cr_installed=false  # both files confirmed 404 — truly absent
            else
                echo "WARN: gh api .coderabbit.yml probe failed with non-404 error; assuming CR installed (fail safe)" >&2
                cr_installed=true
            fi
        else
            echo "WARN: gh api .coderabbit.yaml probe failed with non-404 error; assuming CR installed (fail safe)" >&2
            cr_installed=true
        fi
    fi

    # Read GraphQL document into a variable. `gh api graphql -f query=@file`
    # does NOT read the file — it sends the literal `@filename` string, which
    # the GraphQL parser then chokes on at the leading `@` (directive marker).
    # The canonical pattern is to pass the document body as a string field.
    local query_body
    query_body=$(<"$QUERY_FILE")

    # STALE-recovery state — count consecutive STALE polls for the same HEAD.
    # Reset whenever HEAD advances. After STALE_REREVIEW_POLLS, post one
    # `@coderabbitai review` comment to nudge CR into re-reviewing (idempotent
    # per-HEAD — the trigger fires at most once per head SHA).
    local stale_streak=0
    local stale_head=""
    local stale_rereview_posted_head=""

    local start gh_fails=0
    start=$(date +%s)

    # Option B: parse the GraphQL response with gh's BUNDLED jq (`gh api --jq`)
    # — no standalone `jq` binary required (gh is the only dep). One filter
    # computes every gate field and emits them as a fixed-order, one-per-line
    # stream (17 lines) that the poll loop reads with `mapfile`. The exact jq
    # sub-expressions are the same ones the per-field `jq` calls used before;
    # they're just composed into one program. ORCH_USER is spliced in as a
    # string literal because `gh --jq` (unlike standalone jq) takes no --arg.
    # Field order (index): 0 state · 1 headSha · 2 overflow · 3 testsOob ·
    # 4 perfOob · 5 ciTotal · 6 ciFail · 7 ciPend · 8 ciWarnDowngraded ·
    # 9 dgNames · 10 crState · 11 crFirstLine · 12 crOpen · 13 crStatusState ·
    # 14 crThreadCommentsOnHead · 15 userComments · 16 reviewDecision.
    local GATE_FILTER
    GATE_FILTER='
.data.repository.pullRequest as $pr
| ($pr.headRefOid // "") as $sha
| ([$pr.labels.nodes[]?.name]) as $labels
| ($labels | any(. == "tests-out-of-band")) as $tests
| ($labels | any(. == "perf-out-of-band")) as $perf
| ((($pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes) // [])
   | map(. + {_k: (if .__typename == "CheckRun" then ["CheckRun", (.name // "")]
                   else ["StatusContext", (.context // "")] end)})
   | group_by(._k) | map(sort_by(.startedAt // "") | .[-1]) | map(del(._k))) as $ctx
| ([$ctx[] | select(.isRequired == true)]) as $req
| ([$req[] | select(
      (.__typename == "CheckRun" and .status == "COMPLETED" and ((.conclusion // "") | IN("FAILURE","TIMED_OUT","CANCELLED","ACTION_REQUIRED","STARTUP_FAILURE"))) or
      (.__typename == "StatusContext" and ((.state // "") | IN("FAILURE","ERROR"))))]) as $failing
| ([$failing[] | select(
      ($tests and .__typename == "CheckRun" and .name == "Test-delta gate") or
      ($perf  and .__typename == "CheckRun" and ((.name // "") | startswith("Perf PR-fast"))))]) as $downgraded
| ([$pr.reviews.nodes[] | select(.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]")]) as $crall
| (if ($crall | length) == 0 then "NONE"
   else (([$crall[] | select(.commit.oid == $sha)]) as $cur
         | if ($cur | length) == 0 then "STALE" else ($cur | sort_by(.submittedAt) | .[-1].state) end) end) as $crstate
| (if ($crall | length) == 0 then ""
   else (([$crall[] | select(.commit.oid == $sha)]) as $cur
         | if ($cur | length) > 0 then ($cur | sort_by(.submittedAt) | .[-1].body // "")
           else ($crall | sort_by(.submittedAt) | .[-1].body // "") end) end) as $crbody
| (
    ($pr.state // "UNKNOWN"),
    $sha,
    (((($pr.commits.nodes[0].commit.statusCheckRollup.contexts.pageInfo.hasNextPage // false)
       or ($pr.reviews.pageInfo.hasNextPage // false)
       or ($pr.reviewThreads.pageInfo.hasNextPage // false)
       or ($pr.comments.pageInfo.hasNextPage // false)
       or ($pr.labels.pageInfo.hasNextPage // false)
       or (any($pr.reviewThreads.nodes[]?; .comments.pageInfo.hasNextPage // false)))) | tostring),
    ($tests | tostring),
    ($perf | tostring),
    ($req | length),
    ([$failing[] | select(. as $f | ($downgraded | any(.name == $f.name and .__typename == $f.__typename)) | not)] | length),
    ([$req[] | select(
        (.__typename == "CheckRun" and .status != "COMPLETED") or
        (.__typename == "StatusContext" and ((.state // "") | IN("PENDING","EXPECTED"))))] | length),
    ($downgraded | length),
    ([$downgraded[].name] | join(", ")),
    $crstate,
    (($crbody | split("\n"))[0] // ""),
    ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
        and any(.comments.nodes[]; .author.login == "coderabbitai" or .author.login == "coderabbitai[bot]"))] | length),
    ([$pr.commits.nodes[0].commit.statusCheckRollup.contexts.nodes[]?
      | select(.__typename == "StatusContext" and .context == "CodeRabbit") | .state] | (.[0] // "ABSENT")),
    ([$pr.reviewThreads.nodes[]? | .comments.nodes[]?
      | select((.author.login == "coderabbitai" or .author.login == "coderabbitai[bot]") and (.commit.oid // "") == $sha)] | length),
    (([$pr.comments.nodes[] | select(.author.__typename != "Bot" and ((.author.login // "") | ascii_downcase) != ("__ORCH_USER__" | ascii_downcase))] | length)
     + ([$pr.reviewThreads.nodes[] | select(.isResolved == false and .isOutdated == false
          and any(.comments.nodes[]; .author.__typename != "Bot" and ((.author.login // "") | ascii_downcase) != ("__ORCH_USER__" | ascii_downcase)))] | length)),
    ($pr.reviewDecision // "NONE"),
    ($pr.mergeStateStatus // "UNKNOWN")
  )
'
    GATE_FILTER="${GATE_FILTER//__ORCH_USER__/$ORCH_USER}"

    local p
    for ((p=0; p<MAX_POLLS; p++)); do
        local data
        if ! data=$(gh api graphql \
                       -f owner="$owner" \
                       -f repo="$repo" \
                       -F pr="$prNumber" \
                       -f query="$query_body" \
                       --jq "$GATE_FILTER" 2>&1); then
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gh failed ($gh_fails/3): $data"
            if [ "$gh_fails" -ge 3 ]; then
                echo "GH_API_DOWN"
                return 3
            fi
            # Wall-clock check on failure path — intermittent failures that
            # never hit 3-in-a-row must not exceed MERGE_GATES_TIMEOUT_SECONDS.
            local elapsed_fail=$(( $(date +%s) - start ))
            if [ "$elapsed_fail" -ge "$TIMEOUT_SECONDS" ]; then
                echo "GATES_TIMEOUT"
                return 2
            fi
            if [ "$p" -lt $((MAX_POLLS-1)) ]; then
                sleep "$POLL_INTERVAL"
            fi
            continue
        fi
        gh_fails=0

        # Parse the gh --jq field stream — 17 fixed-order lines (see GATE_FILTER
        # field map above). gh --jq errors already routed through the gh-fail
        # path above; this guards a truncated/partial body → fail closed (retry).
        local fields
        # Strip CR — Windows jq builds (and gh's bundled jq on Windows) emit
        # CRLF, which would leave a trailing \r on every field (e.g. pr_state
        # "OPEN\r" != "OPEN" → spurious return-4).
        data="${data//$'\r'/}"
        mapfile -t fields <<<"$data"
        if [ "${#fields[@]}" -ne 18 ]; then
            # Exactly 18 expected. Any other count (a field value with an embedded
            # newline would inflate it, misaligning fields[n]) → fail closed (CR #511).
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gate filter returned ${#fields[@]} fields (expected 18); transient ($gh_fails/3)"
            if [ "$gh_fails" -ge 3 ]; then echo "GH_API_DOWN"; return 3; fi
            local elapsed_short=$(( $(date +%s) - start ))
            if [ "$elapsed_short" -ge "$TIMEOUT_SECONDS" ]; then echo "GATES_TIMEOUT"; return 2; fi
            if [ "$p" -lt $((MAX_POLLS-1)) ]; then sleep "$POLL_INTERVAL"; fi
            continue
        fi

        # PR state early-exit. Empty/UNKNOWN → return 4 (no-longer-mergeable).
        local pr_state="${fields[0]:-UNKNOWN}"
        if [ "$pr_state" != "OPEN" ]; then
            echo "PR_${pr_state:-UNKNOWN}"
            return 4
        fi

        local head_sha="${fields[1]}"

        # Pagination overflow — fail closed (return 5). The filter already OR's
        # every hasNextPage; a malformed response routes through the gh-fail path.
        local overflow="${fields[2]}"
        if [ "$overflow" = "true" ]; then
            echo "PAGINATION_OVERFLOW"
            return 5
        fi

        # Labels (tests-out-of-band / perf-out-of-band) — the downgrade was
        # already applied inside the filter's ci_fail / ci_warn_downgraded; these
        # are surfaced only for the WARN line below.
        local has_tests_oob="${fields[3]}" has_perf_oob="${fields[4]}"

        # CI — required-only, latest-per-name dedup + label downgrades, all done
        # in the filter. Empty → -1, which fails closed at the integer checks.
        local ci_total="${fields[5]:--1}"
        local ci_fail="${fields[6]:--1}"
        local ci_pend="${fields[7]:--1}"
        local ci_warn_downgraded="${fields[8]:--1}"
        local dg_names="${fields[9]}"

        # Surface every downgraded check on stderr so the operator sees what the
        # label hid. Mirrors the "Skip gates and merge anyway" LOG_WARN pattern.
        if [ "$ci_warn_downgraded" -gt 0 ]; then
            echo "WARN: out-of-band label(s) downgraded ${ci_warn_downgraded} failing check(s) to WARN: ${dg_names}" >&2
        fi

        # CodeRabbit — four-bucket discrimination with body-aware actionable parsing.
        # The filter computed cr_state + the review body's FIRST LINE (current-head
        # review preferred, else most-recent stale — same selection as before).
        # P1 fix per docs/backlog/agent-self-improvement/process.md: body's first
        # line carries "Actionable comments posted: N" — N>0 means CR found real
        # bugs the user should review before any force-merge / timeout-pass.
        local cr_state="${fields[10]}"
        local cr_first_line="${fields[11]}"
        # Extract N from "Actionable comments posted: N". -1 = header not found.
        # First line only (CR's convention) — nested findings may quote the phrase.
        local cr_actionable=-1
        if [ -n "$cr_first_line" ]; then
            local match
            match=$(printf '%s' "$cr_first_line" | grep -oE 'Actionable comments posted:[[:space:]]*[0-9]+' || true)
            if [ -n "$match" ]; then
                cr_actionable=$(printf '%s' "$match" | grep -oE '[0-9]+')
            fi
        fi
        # -1 (filter/parse miss) fails closed at the `cr_open -eq 0` pass check.
        local cr_open="${fields[12]:--1}"
        # CR StatusContext on the head rollup — some CR configs emit only a status
        # (no review) when clean; SUCCESS is a positive signal. "ABSENT" if none.
        local cr_status_state="${fields[13]}"
        # C4 prong 2: count of CR review-thread comments anchored to the current
        # head — positive evidence CR actively reviewed this commit (vs a bare
        # placeholder StatusContext). The NONE branch uses it to gate the pass.
        local cr_thread_comments_on_head="${fields[14]:--1}"

        # User comments (non-bot, non-self) — -1 fails closed at `user -eq 0`.
        local user="${fields[15]:--1}"

        # reviewDecision
        local review_decision="${fields[16]:-NONE}"
        local review_pass=false
        case "$review_decision" in
            APPROVED|NONE) review_pass=true ;;
        esac

        local cr_pass=false
        local cr_state_print="$cr_state"
        case "$cr_state" in
            APPROVED)
                # Approval on the current head is always a pass, regardless of body shape.
                cr_pass=true
                cr_state_print="APPROVED"
                ;;
            COMMENTED)
                # On-head COMMENTED. Block when CR reported actionable findings (N>0);
                # pass when CR explicitly said 0 actionable; pass when no Actionable
                # header found (body is empty / non-CR-shape / older CR template).
                if [ "$cr_actionable" -gt 0 ]; then
                    cr_pass=false
                    cr_state_print="COMMENTED (${cr_actionable} actionable — block)"
                else
                    cr_pass=true
                    if [ "$cr_actionable" = "0" ]; then
                        cr_state_print="COMMENTED (0 actionable)"
                    else
                        cr_state_print="COMMENTED (no Actionable header)"
                    fi
                fi
                ;;
            STALE)
                # CR reviewed a prior commit. Discriminate via Actionable count from
                # that stale body. STALE_WITH_FINDINGS NEVER passes on timeout (would
                # discard real CR feedback the user hasn't seen). STALE_CLEAN passes
                # on timeout (the prior review was clean; current commit likely still
                # clean modulo new edits). STALE_UNKNOWN treated as STALE_WITH_FINDINGS
                # to be safe — caller can't distinguish "0 actionable" from "no header".
                #
                # H16: STALE_RESOLVED — when CR's prior review found N>0 findings BUT
                # all CR review threads are now resolved AND CR's StatusContext on the
                # current head is SUCCESS, CR has re-evaluated the current commit
                # (resolving threads is its accept signal) without re-issuing a fresh
                # review body. This is the dominant case for small fixup commits where
                # CR accepts the addressing change via thread-resolution rather than
                # posting a new "Actionable comments posted: 0" review. The merge-gate
                # used to wedge here forever; now we treat it as pass. Requires BOTH
                # signals (open=0 AND status=SUCCESS) — open=0 alone could mean the
                # user manually resolved (no CR judgement); status=SUCCESS alone could
                # be a stale placeholder. Together they're a CR-driven accept.
                if [ "$cr_actionable" -gt 0 ] && [ "$cr_open" -eq 0 ] && [ "$cr_status_state" = "SUCCESS" ]; then
                    cr_pass=true
                    cr_state_print="STALE_RESOLVED (${cr_actionable} actionable on prior commit, all threads resolved + status SUCCESS — pass)"
                elif [ "$cr_actionable" -gt 0 ]; then
                    cr_pass=false
                    cr_state_print="STALE_WITH_FINDINGS (${cr_actionable} actionable on prior commit — block + surface review)"
                elif [ "$cr_actionable" = "0" ]; then
                    cr_pass=true
                    cr_state_print="STALE_CLEAN (0 actionable on prior commit — pass)"
                else
                    cr_pass=false
                    cr_state_print="STALE_UNKNOWN (no Actionable header — treat as block per safe-default policy)"
                fi
                # STALE auto-recovery: when CR sits on a STALE blocking state for
                # ≥STALE_REREVIEW_POLLS on the same HEAD, post `@coderabbitai review`
                # once per HEAD to nudge a re-review (idempotent — dedups on head_sha).
                if [ "$cr_pass" = false ] && [ "$STALE_REREVIEW_POLLS" -gt 0 ]; then
                    if [ "$stale_head" = "$head_sha" ]; then
                        stale_streak=$((stale_streak + 1))
                    else
                        stale_head="$head_sha"
                        stale_streak=1
                    fi
                    if [ "$stale_streak" -ge "$STALE_REREVIEW_POLLS" ] && \
                       [ "$stale_rereview_posted_head" != "$head_sha" ]; then
                        echo "WARN: STALE on HEAD ${head_sha:0:8} for $stale_streak polls; posting @coderabbitai review to nudge re-review." >&2
                        if gh pr comment "$prNumber" --body "@coderabbitai review" >/dev/null 2>&1; then
                            stale_rereview_posted_head="$head_sha"
                            echo "INFO: @coderabbitai review trigger posted on HEAD ${head_sha:0:8}." >&2
                        else
                            echo "WARN: gh pr comment failed posting @coderabbitai review; will retry next STALE_REREVIEW_POLLS window." >&2
                        fi
                    fi
                fi
                ;;
            NONE)
                if [ "$cr_installed" != true ]; then
                    # Repo doesn't have CodeRabbit installed — NONE is the steady state.
                    cr_pass=true
                elif [ "$cr_status_state" = "SUCCESS" ] && [ "$cr_thread_comments_on_head" -gt 0 ]; then
                    # C4 prong 2: status-SUCCESS PLUS at least one CR review-thread
                    # comment on the current head. CR has actively reviewed this
                    # commit — placeholder status is corroborated by real review
                    # activity. This is the safe pass path. The previous rule
                    # (status-SUCCESS alone) let draft-PR bypass slip through:
                    # CR's auto_review.drafts:false skipped the review but its
                    # StatusContext placeholder still fired SUCCESS.
                    cr_pass=true
                    cr_state_print="NONE+status-SUCCESS+inline-evidence (${cr_thread_comments_on_head} CR comment(s) on head)"
                elif [ "$cr_status_state" = "SUCCESS" ] && [ "$p" -ge "$CR_GRACE_POLLS" ]; then
                    # Status-SUCCESS but zero inline evidence on current head. Two
                    # possible causes: (a) a status-only CR config (rare; CR's
                    # default emits both status + review), or (b) CR's placeholder
                    # fired without a real review (the C4 bypass). After the grace
                    # window we fall through to pass so the loop never wedges on a
                    # status-only config, but the WARN names the suspicious shape.
                    echo "WARN: CodeRabbit status=SUCCESS but no inline CR comments on head after grace ($CR_GRACE_POLLS polls); possible status-only config OR C4 bypass." >&2
                    cr_pass=true
                    cr_state_print="NONE+status-SUCCESS+no-inline-evidence (grace expired — assume status-only)"
                elif [ "$cr_status_state" = "SUCCESS" ]; then
                    # Status fired SUCCESS, no inline evidence yet, still within
                    # grace. Wait — a real CR review on a freshly-flipped-ready PR
                    # often lands a poll or two after the placeholder. Distinct
                    # from NONE+pending so the operator can see the placeholder.
                    cr_state_print="NONE+status-SUCCESS-waiting-for-inline (poll $((p+1))/$CR_GRACE_POLLS)"
                elif [ "$p" -ge "$CR_GRACE_POLLS" ]; then
                    # Grace window elapsed; CR never started. Log + fall through to pass
                    # so the loop is never wedged by a stuck integration.
                    echo "WARN: CodeRabbit grace window ($CR_GRACE_POLLS polls) expired without a review or SUCCESS status; treating NONE as pass." >&2
                    cr_pass=true
                    cr_state_print="NONE+grace-expired"
                else
                    cr_state_print="NONE+pending (poll $((p+1))/$CR_GRACE_POLLS)"
                fi
                ;;
        esac

        # Reset STALE streak whenever the state leaves the BLOCKING-STALE
        # family. Any non-STALE cr_state breaks consecutive — and so do
        # passing STALE variants (STALE_CLEAN / STALE_RESOLVED) where
        # cr_pass=true, since the re-review trigger only makes sense for
        # blocking-STALE polls. Without `|| cr_pass=true`, an intermittent
        # STALE_WITH_FINDINGS -> STALE_RESOLVED -> STALE_WITH_FINDINGS
        # pattern would accumulate streak across the passing intervals.
        if [ "$cr_state" != "STALE" ] || [ "$cr_pass" = true ]; then
            stale_streak=0
            stale_head=""
        fi

        printf 'Poll %d/%d — CI: %d/%d pass (%d fail, %d pending, %d warn-downgraded) | CodeRabbit: %s (%d open) | User: %d | reviewDecision: %s\n' \
            $((p+1)) "$MAX_POLLS" $((ci_total - ci_fail - ci_pend - ci_warn_downgraded)) "$ci_total" \
            "$ci_fail" "$ci_pend" "$ci_warn_downgraded" \
            "$cr_state_print" "$cr_open" "$user" "$review_decision"

        # H1: APPROVED CR review passes unconditionally per AGENTS.md § Merge
        # gates § CodeRabbit ("APPROVED → pass unconditionally (approval trumps
        # body)"). Previously the pass-check always required cr_open == 0, so
        # an APPROVED review on the current head + any unresolved non-outdated
        # CR thread (even one CR itself left for context) wedged the gate.
        # Decompose into an explicit `cr_open_blocks` so the intent is legible.
        local cr_open_blocks=false
        if [ "$cr_state" != "APPROVED" ] && [ "$cr_open" -ne 0 ]; then
            cr_open_blocks=true
        fi

        if [ "$ci_fail" -eq 0 ] && [ "$ci_pend" -eq 0 ] && \
           [ "$cr_pass" = true ] && [ "$cr_open_blocks" = false ] && \
           [ "$user" -eq 0 ] && [ "$review_pass" = true ]; then
            # Diagnostic: surface GitHub's mergeStateStatus alongside our pass
            # decision so the operator can correlate when GH says BLOCKED while
            # our gates pass (typically branch-protection summary-only / stale
            # GH mergeability cache — REST squash-merge still works).
            local gh_merge_state="${fields[17]:-UNKNOWN}"
            if [ "$gh_merge_state" != "CLEAN" ] && [ "$gh_merge_state" != "UNSTABLE" ] && [ "$gh_merge_state" != "UNKNOWN" ]; then
                echo "INFO: merge-gates pass; GitHub mergeStateStatus=$gh_merge_state may be stale or branch-protection summary-only. REST squash-merge contract still applies." >&2
            fi
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
# H2: positive-check fallback. `gh pr ready` returns non-zero with an English
# stderr message ("not in draft state" / "already marked ready") when called
# against an already-non-draft PR. Matching on English text breaks if `gh`
# updates its wording, ships a localised build, or the user is on a locale-
# overridden CLI. Fall back to a positive state probe via
# `gh pr view --json isDraft`: if the PR is observably non-draft, the
# original `gh pr ready` failure was the benign "already ready" case and we
# can return 0. Any other failure surfaces as exit 6.
gh_pr_ready_idempotent() {
    local prNumber="${1:?gh_pr_ready_idempotent: pr_number required}"
    local out
    if ! out=$(gh pr ready "$prNumber" 2>&1); then
        # Fast path — known English phrases. Cheaper than the extra API call
        # and preserves backward compatibility with the prior contract.
        case "$out" in
            *"not in draft state"*|*"already marked ready"*)
                return 0
                ;;
        esac
        # Positive-check fallback: probe the PR's actual draft state. If it's
        # already non-draft, the `gh pr ready` failure was benign. Robust
        # against `gh` wording changes + locale variation + CLI version drift.
        local is_draft
        if is_draft=$(gh pr view "$prNumber" --json isDraft --jq .isDraft 2>/dev/null); then
            if [ "$is_draft" = "false" ]; then
                return 0
            fi
        fi
        echo "$out" >&2
        return 6
    fi
}

# ----------------------------------------------------------------------------
# CLI entry point — only when invoked directly (not sourced).
# ----------------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    poll_merge_gates "$@"
fi
