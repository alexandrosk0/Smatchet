# Plan: Merge Gates — CI green + CodeRabbit + user comments before merge

## Context

Current ship-loop: `diagnose → fix → build → commit → push → open PR → squash-merge → cleanup`. No CI wait, no comment-resolution gate. Post-ship offers 4 options: Manual verify / Review PR / Squash-merge / Done.

Add a gate-check step between "open PR" and "squash-merge" that polls three conditions via a single GraphQL query, blocks the merge until all clear (or user overrides). Bash-only runtime; tests via `bats`. No C++ surface.

**Environment**:
- `SKIP_MERGE_GATES=true` — bypass all gates (session-init override).
- `MERGE_GATES_POLL_INTERVAL` — override poll interval seconds (default 60, tests use 0).
- `MERGE_GATES_MAX_POLLS` — override max poll count (default 60, tests use 1 for single-shot).

---

## Scope and contract boundaries

This plan applies **only to the orchestrator + `git-janitor` running in the main user session**. Three contracts stay unchanged:

1. **Spawned-child agents** (`handoff-implementer`, `pr-iterator`) — still forbidden from `gh pr ready` and any merge call. They write `--draft` PRs and exit. `agents/handoff-implementer.md` § Hard rules and `agents/pr-iterator.md` § Hard rules are not touched.
2. **Handoff envelope "PR draft requirement"** — refers to the spawned-child harness, not the orchestrator. Clarification edit only: rename header to "Spawned-child PR draft requirement" so the boundary is explicit.
3. **Orchestrator + `git-janitor`** — gain auto-`gh pr ready` + REST-merge under one trigger only: user explicitly picked post-ship option 3 ("Wait for gates and merge") **or** said "merge when green" in-session. This is per-PR authorization, not a blanket policy change.

Cross-link: `the deleted handoff-envelope section` clarification edit listed in Files-changed.

---

## 1. AGENTS.md — ship-loop sequence

Replace ship-loop (current line ~77):
```
BEFORE: diagnose → fix → build → commit → push → open PR → squash-merge → cleanup
AFTER:  diagnose → fix → build → commit → push → open PR → [gate-check] → squash-merge → cleanup
```

`[gate-check]` polls every 60s, 60-poll cap (~60min wall-clock). Auto-merge on pass when user has authorised it (option 3 or "merge when green"). Halt + `AskUserQuestion` on block / timeout / `gh` API failure / PR closed-externally / pagination overflow.

---

## 2. AGENTS.md — merge gates section (new)

```
### Merge gates

Before squash-merge, orchestrator polls three conditions via one GraphQL query
(`gh api graphql`):

1. **CI**: every required check (`isRequired(pullRequestNumber: $pr) == true`)
   in `pullRequest.commits(last:1).commit.statusCheckRollup.contexts` must
   reach a passing terminal state.
   - CheckRun: `status == "COMPLETED"` AND `conclusion in {SUCCESS, NEUTRAL, SKIPPED}`.
     Conclusions `FAILURE, TIMED_OUT, CANCELLED, ACTION_REQUIRED, STARTUP_FAILURE`
     block. `STALE` passes (GitHub treats as superseded). Any non-COMPLETED status
     is pending.
   - StatusContext: `state == "SUCCESS"` passes. `FAILURE, ERROR` block.
     `PENDING, EXPECTED` are pending. Default rule: any required context with
     `state != "SUCCESS"` blocks; status text decides fail vs pending.
   - Non-required checks are ignored.

2. **CodeRabbit**: identity match is `author.login in {"coderabbitai",
   "coderabbitai[bot]"}` (GraphQL may strip the `[bot]` suffix; REST keeps it —
   the existing `coderabbit-triage` agent and its test fixtures use the
   `[bot]` form, so we match both for safety). CodeRabbit state is computed
   in three buckets:
   - `NONE` — no review by CodeRabbit ever submitted → **pass** (lets repos
     without CodeRabbit installed work without `SKIP_MERGE_GATES`).
   - `STALE` — CodeRabbit reviews exist but none submitted against the
     current `headRefOid` → **block** (force-push race; old approval is not
     valid for new code).
   - latest review's state on current `headRefOid` ∈ {`APPROVED`, `COMMENTED`}
     → pass; ∈ {`CHANGES_REQUESTED`, `DISMISSED`} → block.
   Additionally: zero unresolved non-outdated review threads contain a
   CodeRabbit comment (under the same login-match rule).

3. **User comments**: zero unresolved non-outdated review threads with any
   non-bot non-self comment, AND zero conversation-tab comments from a
   non-bot non-self author. Bot detection uses GraphQL
   `author.__typename == "Bot"` (covers all integrations). Self matched via
   `$ORCH_USER` lower-cased on both sides.

Additional pass conditions:
- `pullRequest.state == "OPEN"` (early-exit on closed/merged-externally).
- `pullRequest.reviewDecision in {"APPROVED", null}` (blocks on
  `REVIEW_REQUIRED` / `CHANGES_REQUESTED`). Note: branch protection
  enforces this server-side too; our gate is meaningful when branch
  protection is absent but reviewers were requested.
- **Pagination ceiling**: GitHub GraphQL caps connections at 100.
  The query also fetches `pageInfo.hasNextPage` for every connection
  (checks, reviews, reviewThreads, per-thread comments, conversation
  comments). Any `hasNextPage == true` → block with `PAGINATION_OVERFLOW`
  and halt for user decision. Pagination is treated as a hard block
  rather than silently truncated — a missed blocker on page 2 would
  defeat the gate.

Conflict and missing-required-check enforcement is delegated to GitHub:
the merge call errors on either; we surface the error to user. No
client-side duplicate.

`$ORCH_USER` resolved at session init via `gh api user --jq .login`.

Override: `SKIP_MERGE_GATES=true` at session init bypasses all gates. No
per-merge skip. Subagent propagation: orchestrator must explicitly add
`SKIP_MERGE_GATES` to any delegated `git-janitor` invocation's env (it does
not auto-inherit through the subagent boundary).

Status line per poll:
  "Poll 3/60 — CI: 5/8 pass (1 fail, 2 pending) | CodeRabbit: CHANGES_REQUESTED (2 threads) | User: 1 | reviewDecision: APPROVED"

Auto-convert draft → ready (`gh pr ready`) when gates pass, but only when
the user explicitly authorised this PR for merge (post-ship option 3 or
"merge when green" in-session). Otherwise gates pass status is reported
and the orchestrator stops without flipping draft state.
```

---

## 3. AGENTS.md — post-ship protocol

Replace current 4 AskUserQuestion options — option 3 swaps "Squash-merge" → "Wait for gates and merge":

1. **Manual verify** (unchanged)
2. **Review PR** (unchanged)
3. **Wait for gates and merge** — orchestrator polls every 60s. On pass, `gh pr ready` + REST squash-merge + cleanup. On timeout / block / `gh` down / PR closed-externally / pagination overflow → `AskUserQuestion` (see § Halt prompts below).
4. **Done** — PR stays draft (unchanged)

Skip-condition: user said "merge when green" / "ship it and stop" → enter option 3 implicitly.

### Halt prompts (codes 1–5)

Different return codes get different prompt sets — "Skip gates and merge" is meaningless on a closed PR.

| Code | Meaning | Prompt options |
|---|---|---|
| 1 | Gates still blocked after MAX_POLLS | "Skip gates and merge anyway" / "Keep waiting (extend poll)" / "Abandon" |
| 2 | Wall-clock timeout (≥3600s) | "Skip gates and merge anyway" / "Keep waiting" / "Abandon" |
| 3 | `gh` API failed 3 consecutive polls | "Retry now" / "Skip gates and merge anyway" / "Abandon" |
| 4 | PR `CLOSED` or `MERGED` externally | "Abandon (PR no longer mergeable)" only — no skip option |
| 5 | Pagination overflow (any `hasNextPage`) | "Abandon (manual review required — pagination)" / "Skip gates and merge anyway (acknowledge risk)" |

Any "Skip gates and merge anyway" choice logs `LOG_WARN "user skipped gates: code=<n>, reason=<text>"` before proceeding.

---

## 4. git-janitor.md — gate check before merge

**Current flow**: verify mergeable → REST squash-merge → delete branch → plan revision

**New flow**: verify mergeable → `poll_merge_gates` → (if authorised) `gh pr ready` → REST squash-merge → delete branch → plan revision

### GraphQL query — `scripts/dev/merge-gates.graphql`

```graphql
query($owner: String!, $repo: String!, $pr: Int!) {
  repository(owner: $owner, name: $repo) {
    pullRequest(number: $pr) {
      state                  # OPEN / CLOSED / MERGED — exit poller when not OPEN
      isDraft
      reviewDecision         # APPROVED / REVIEW_REQUIRED / CHANGES_REQUESTED / null
      headRefOid
      headRefName            # branch name for post-merge `git refs/heads/<name>` DELETE
      commits(last: 1) {
        nodes { commit { statusCheckRollup { contexts(first: 100) {
          pageInfo { hasNextPage }
          nodes {
            __typename
            ... on CheckRun     { name conclusion status isRequired(pullRequestNumber: $pr) }
            ... on StatusContext { context state isRequired(pullRequestNumber: $pr) }
          }
        }}}}
      }
      reviews(first: 100) {
        pageInfo { hasNextPage }
        nodes {
          author { login __typename }
          state submittedAt
          commit { oid }     # force-push race: only reviews on headRefOid count
        }
      }
      reviewThreads(first: 100) {
        pageInfo { hasNextPage }
        nodes {
          isResolved isOutdated
          comments(first: 20) {
            pageInfo { hasNextPage }
            nodes { author { login __typename } }
          }
        }
      }
      comments(first: 100) {
        pageInfo { hasNextPage }
        nodes { author { login __typename } }
      }
    }
  }
}
```

### Bash poller

```bash
poll_merge_gates() {
    # Inputs: $owner $repo $prNumber
    # Globals: $ORCH_USER (set at session init)
    # Env knobs (tests): MERGE_GATES_POLL_INTERVAL (default 60), MERGE_GATES_MAX_POLLS (default 60).
    local POLL_INTERVAL="${MERGE_GATES_POLL_INTERVAL:-60}"
    local MAX_POLLS="${MERGE_GATES_MAX_POLLS:-60}"
    local TIMEOUT_SECONDS=3600
    local start gh_fails=0
    start=$(date +%s)
    local q="scripts/dev/merge-gates.graphql"

    for ((p=0; p<MAX_POLLS; p++)); do
        local data
        # NOTE: `gh api graphql -f query=@file` does NOT read the file — it
        # sends the literal `@filename` string, which the GraphQL parser
        # then rejects at the leading `@` (directive marker). Read the
        # document into a variable first; pass as a plain string field.
        local query_body
        query_body=$(<"$q")
        if ! data=$(gh api graphql -f owner="$owner" -f repo="$repo" \
                       -F pr="$prNumber" -f query="$query_body" 2>&1); then
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gh failed ($gh_fails/3): $data"
            [ "$gh_fails" -ge 3 ] && { echo "GH_API_DOWN"; return 3; }
            [ "$p" -lt $((MAX_POLLS-1)) ] && sleep "$POLL_INTERVAL"
            continue
        fi
        gh_fails=0
        local pr
        pr=$(jq '.data.repository.pullRequest' <<<"$data")

        # PR state early-exit (closed/merged externally)
        local pr_state
        pr_state=$(jq -r '.state' <<<"$pr")
        if [ "$pr_state" != "OPEN" ]; then
            echo "PR_$pr_state"; return 4
        fi
        local head_sha
        head_sha=$(jq -r '.headRefOid' <<<"$pr")

        # Pagination overflow (any connection capped at 100 with more pages)
        local overflow
        overflow=$(jq '
            ((.commits.nodes[0].commit.statusCheckRollup.contexts.pageInfo.hasNextPage // false)
             or (.reviews.pageInfo.hasNextPage // false)
             or (.reviewThreads.pageInfo.hasNextPage // false)
             or (.comments.pageInfo.hasNextPage // false)
             or (any(.reviewThreads.nodes[]?; .comments.pageInfo.hasNextPage // false)))
        ' <<<"$pr")
        if [ "$overflow" = "true" ]; then
            echo "PAGINATION_OVERFLOW"; return 5
        fi

        # CI — required-only. Pass = SUCCESS terminal. Fail = blocking terminal. Pending = anything else.
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

        # CodeRabbit — three-bucket state (NONE / STALE / latest-on-head)
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

        # User comments — typename-based bot filter, both surfaces, login lower-cased
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

        # reviewDecision — relevant when branch protection absent but reviewers requested
        local review_decision
        review_decision=$(jq -r '.reviewDecision // "NONE"' <<<"$pr")
        local review_pass=false
        case "$review_decision" in
            APPROVED|NONE) review_pass=true ;;
        esac

        local cr_pass=false
        case "$cr_state" in APPROVED|COMMENTED|NONE) cr_pass=true ;; esac

        echo "Poll $((p+1))/$MAX_POLLS — CI: $((ci_total-ci_fail-ci_pend))/$ci_total pass ($ci_fail fail, $ci_pend pending) | CodeRabbit: $cr_state ($cr_open open) | User: $user | reviewDecision: $review_decision"

        if [ "$ci_fail" -eq 0 ] && [ "$ci_pend" -eq 0 ] && \
           [ "$cr_pass" = true ] && [ "$cr_open" -eq 0 ] && \
           [ "$user" -eq 0 ] && [ "$review_pass" = true ]; then
            echo "GATES_PASSED"; return 0
        fi

        [ $(( $(date +%s) - start )) -ge "$TIMEOUT_SECONDS" ] && { echo "GATES_TIMEOUT"; return 2; }
        [ "$p" -lt $((MAX_POLLS-1)) ] && sleep "$POLL_INTERVAL"
    done
    return 1
}
```

**Return codes**:
- 0 — gates passed
- 1 — gates still blocked at MAX_POLLS
- 2 — timeout (≥3600s wall-clock exceeded before MAX_POLLS hit)
- 3 — gh API down (3 consecutive failures)
- 4 — PR closed or merged externally
- 5 — pagination overflow (any connection has more pages)
- 6 — `gh pr ready` unknown failure (set by `gh_pr_ready_idempotent` helper, not `poll_merge_gates`)

Codes 1 and 2 are distinct: 1 = "poll budget exhausted" (likely if interval > 60s or pre-empted); 2 = "wall-clock budget exhausted" (the common case under default settings). Either is "still blocked", but the status line at the calling agent reports which budget tripped.

Conflicts and missing-required-checks are not separate return codes — the REST merge call errors on them; the caller surfaces the error.

### AskUserQuestion shell shim

Tests need to stub `AskUserQuestion` (an MCP tool, not a shell command). The poller's halt prompts go through a bash function wrapper:

```bash
# scripts/dev/merge-gates-prompt.sh
ask_user_question() {
    # Stubbable. Real implementation prints options + reads a number; the
    # orchestrator overrides this with an `AskUserQuestion` MCP invocation.
    # Bats tests override the function in setup() to return a canned answer
    # via $MERGE_GATES_TEST_ANSWER.
    if [ -n "${MERGE_GATES_TEST_ANSWER:-}" ]; then
        printf '%s\n' "$MERGE_GATES_TEST_ANSWER"; return 0
    fi
    # Real interactive path: pass options on stdin, return user choice on stdout.
    # ... (orchestrator-injected implementation) ...
}
```

The orchestrator-side wiring (`AskUserQuestion` MCP call) lives in the `git-janitor` agent prose, not the script — the script only declares the contract.

### Integration in git-janitor

```
0. Source helper (once per session):
     source scripts/dev/merge-gates-prompt.sh   # declares ask_user_question
1. Session init (once):
   - gh auth status || FAIL_LOUD
   - ORCH_USER=$(gh api user --jq .login)
2. verify_mergeable; capture $branch from PR data (GraphQL headRefName,
   or `gh pr view --json headRefName -q .headRefName`).
3. [ "$SKIP_MERGE_GATES" = true ] && goto step 6
4. poll_merge_gates; rc=$?
5. case "$rc" in
     0)  continue ;;
     1|2|3) ask_user_question "Skip gates and merge anyway / Keep waiting / Abandon"
            on skip:    LOG_WARN "user skipped gates: code=$rc"; continue
            on wait:    extend budget — double MAX_POLLS and reset $start, then
                        re-enter poll_merge_gates; loop terminates only on a
                        different rc or another user decision
            on abandon: halt ;;
     4)  ask_user_question "Abandon"   # PR no longer mergeable; no skip option
         halt ;;
     5)  ask_user_question "Abandon (pagination — manual review) / Skip and merge anyway"
            on skip:    LOG_WARN "user skipped gates: code=5 (pagination)"; continue
            on abandon: halt ;;
   esac
6. If user authorised this PR for merge AND isDraft from last poll:
     run gh_pr_ready_idempotent "$prNumber"   # see error-filter snippet below
7. REST squash-merge (do NOT use `gh pr merge --delete-branch`):
     gh api -X PUT "repos/$owner/$repo/pulls/$prNumber/merge" -f merge_method=squash
     # then delete the branch ref separately, using headRefName captured in step 2:
     gh api -X DELETE "repos/$owner/$repo/git/refs/heads/$branch" 2>/dev/null || true
   Rationale: matches existing git-janitor.md § Hard refusals line 53 — REST
   merge bypasses the local-checkout requirement of `gh pr merge` and avoids
   Windows worktree quirks.
8. plan revision
```

**`gh pr ready` error handling**: do not blanket-suppress. If the PR is already non-draft, `gh pr ready` exits non-zero with a known message; filter that case only. All other errors halt with the gh error surfaced. Wrap as a helper:

```bash
gh_pr_ready_idempotent() {
    local prNumber="$1"
    local out
    if ! out=$(gh pr ready "$prNumber" 2>&1); then
        case "$out" in
            *"not in draft state"*|*"already marked ready"*) return 0 ;;   # idempotent — OK
            *) echo "$out"; return 6 ;;
        esac
    fi
}
```

Return code 6 (`gh pr ready` unknown failure) is a halt — surface to user, do not auto-merge.

**Notes**:
- `cr_state == NONE` (no CodeRabbit review ever) passes the gate so repos without CodeRabbit installed work without `SKIP_MERGE_GATES`. `cr_state == STALE` (CodeRabbit existed but not on current SHA) blocks — old approvals are not valid after force-push.
- Force-push race for CodeRabbit reviews handled by `review.commit.oid == headRefOid` filter.
- Force-push race for CI: `commits(last:1)` always shows current-SHA checks; no separate handling needed.
- Bot filter for user comments uses `author.__typename != "Bot"` — covers all GitHub bot integrations without enumerating logins.
- Login comparison is lower-cased on both sides to handle case-variant logins.
- Pagination overflow is a hard block, not a silent truncation. Edge case for very active PRs (>100 comments / threads); user must skip-with-acknowledgement to proceed.
- Conflicts, missing required checks, and branch-protection rules are enforced by GitHub on the REST merge call. We do not duplicate.

---

## 5. AGENTS.md — Handoff envelope clarification

The "PR draft requirement" subsection currently reads "Every PR opened by the harness is `--draft`." In context it means **the spawned-child harness**, but the wording lets the orchestrator's new auto-merge path be mis-read as a contract violation. Rename + clarify:

```
### Spawned-child PR draft requirement

Every PR opened by a spawned `claude` child (`handoff-implementer`,
`pr-iterator`) is `--draft`. The user marks ready-for-review only after
auditing the diff. The spawned child never calls `gh pr ready`,
`gh pr merge`, `gh api …/merge`, never closes / reopens PRs, and never
pushes to a non-`agent/*` branch. The orchestrator running in the user's
main session may auto-`gh pr ready` + REST-merge under § Merge gates
when the user has explicitly authorised the PR for merge.
```

No change to `handoff-implementer.md` § Hard rules line 91-92 — they already reference the spawned-child case correctly. Re-read pass only.

---

## Files changed

| File | Change |
|---|---|
| `AGENTS.md` | Ship-loop add `[gate-check]`; new Merge-gates section; post-ship option 3 swap + per-code halt prompts; Handoff-envelope subsection rename + clarification |
| `agents/core/git-janitor.md` | Insert `poll_merge_gates` flow before REST squash-merge; session-init prerequisites (`ORCH_USER`); `ask_user_question` halt routing per return code; idempotent `gh pr ready` error filter |
| `scripts/dev/merge-gates.graphql` | GraphQL document with pagination probes |
| `scripts/dev/merge-gates-prompt.sh` | `ask_user_question` shell shim (orchestrator overrides for real prompts; bats overrides for tests) |
| `tests/bats/merge_gates.bats` | Drive `poll_merge_gates` against fixture JSONs |
| `tests/fixtures/merge_gates_*.json` | 8 fixtures (see Verification) |

---

## Verification

`bats` is the runtime test harness. Stub `gh` via `PATH` shim — fixture-driven. Default per-test env: `MERGE_GATES_POLL_INTERVAL=0` and `MERGE_GATES_MAX_POLLS=1` (single-shot, no real sleeps). Per-test overrides allowed — e.g. "3 consecutive gh failures → return 3" sets `MERGE_GATES_MAX_POLLS=3`; "synthetic ≥3601s elapsed → return 2" mocks `date +%s` to return `start+3601`.

### Fixtures (8 files in `tests/fixtures/`)

1. `merge_gates_pass.json` — all gates clear; includes skipped/neutral CheckRun + outdated threads + bot comments + self comment + STALE conclusion (none should block)
2. `merge_gates_ci_fail.json` — required CheckRun with conclusion `FAILURE`; separate variant with `StatusContext.state == "ERROR"`
3. `merge_gates_ci_pending.json` — required CheckRun status `IN_PROGRESS`; separate variant with `StatusContext.state == "EXPECTED"`
4. `merge_gates_cr_changes.json` — coderabbitai latest review `CHANGES_REQUESTED` on current head + 2 unresolved threads
5. `merge_gates_cr_stale.json` — coderabbitai `APPROVED` but on old SHA (force-push race); must block (STALE), not pass
6. `merge_gates_user_comment.json` — non-bot non-self conversation comment + unresolved thread with user comment (separate from fixture 4 to isolate the gate)
7. `merge_gates_state.json` — covers `state=CLOSED`, `state=MERGED`, `reviewDecision=REVIEW_REQUIRED`, `reviewDecision=CHANGES_REQUESTED` (jq overlays select variant)
8. `merge_gates_pagination.json` — `pageInfo.hasNextPage=true` on each connection in turn (overlays select which)

### Test cases in `merge_gates.bats`

```bash
# Pass / block by gate
@test "all gates pass → return 0"                                 # fixture 1
@test "CI conclusion FAILURE → return 1"                          # fixture 2
@test "CI StatusContext state ERROR → return 1"                   # fixture 2 variant
@test "CI pending IN_PROGRESS → return 1"                         # fixture 3
@test "CI StatusContext state EXPECTED → return 1 (pending)"      # fixture 3 variant
@test "CodeRabbit CHANGES_REQUESTED on current head → return 1"   # fixture 4
@test "user conversation comment → return 1"                      # fixture 6
@test "user unresolved thread comment → return 1"                 # fixture 6 variant

# CodeRabbit identity normalization
@test "coderabbitai login matches"                                # fixture 4
@test "coderabbitai[bot] login matches"                           # fixture 4 variant (rewrite login)
@test "stale CodeRabbit APPROVED on old SHA → return 1 (STALE)"   # fixture 5
@test "no CodeRabbit review ever → cr_state=NONE → pass"          # fixture 1 (empty reviews)

# Early-exit + reviewDecision + pagination
@test "PR state=CLOSED → return 4"                                # fixture 7
@test "PR state=MERGED → return 4"                                # fixture 7
@test "reviewDecision=REVIEW_REQUIRED → return 1"                 # fixture 7
@test "reviewDecision=CHANGES_REQUESTED → return 1"               # fixture 7
@test "reviewDecision=APPROVED → contributes to pass"             # fixture 1
@test "contexts.pageInfo.hasNextPage=true → return 5"             # fixture 8
@test "reviews.pageInfo.hasNextPage=true → return 5"              # fixture 8
@test "reviewThreads.pageInfo.hasNextPage=true → return 5"        # fixture 8
@test "comments.pageInfo.hasNextPage=true → return 5"             # fixture 8
@test "per-thread comments.pageInfo.hasNextPage=true → return 5"  # fixture 8

# Filter edge cases
@test "Bot-typename comment does not block"                       # fixture 1
@test "outdated thread does not block"                            # fixture 1
@test "self comment (case-variant login) does not block"          # fixture 1
@test "skipped/neutral/STALE conclusion does not block"           # fixture 1

# Operational
@test "gh_pr_ready_idempotent: already-ready exits 0"             # mock gh exit nonzero w/ "not in draft state"
@test "gh_pr_ready_idempotent: unknown error returns 6"           # mock gh exit nonzero w/ unknown msg
@test "SKIP_MERGE_GATES=true bypasses poller entirely"             # git-janitor flow integration
@test "SKIP_MERGE_GATES does NOT auto-inherit through subagent"   # explicit env-passing test

# Infra
@test "3 consecutive gh failures → return 3"                      # mock gh exit 1
@test "synthetic ≥3601s elapsed → return 2"                       # mock date
@test "MAX_POLLS exhausted with budget remaining → return 1"      # MAX_POLLS=1, blocking fixture
@test "user-skip path emits LOG_WARN with code"                   # MERGE_GATES_TEST_ANSWER=skip
@test "code-4 halt offers no skip option"                         # MERGE_GATES_TEST_ANSWER asserts options
```

### Manual smoke

1. Open a real PR with red CI → trigger gate-check → expect block + status + halt prompt
2. Resolve all gates → expect pass + auto-`gh pr ready` + REST squash-merge
3. `SKIP_MERGE_GATES=true` → expect immediate REST merge without polling
4. Force-push to a PR with an existing CodeRabbit `APPROVED` → expect `STALE` block

---

## Out of scope

Deferred — do not implement under this plan:

- **Branch protection management** — server-side branch protection rules are GitHub's responsibility; this gate is meaningful when protection is absent.
- **Auto-resolve threads on user request** — gate only checks resolution state, never mutates.
- **Multi-repo / merge-queue / queued-merge groups** — single-PR squash-merge only.
- **`gh pr merge --auto` flag** — uses GitHub's native auto-merge; our poller is the alternative and intentionally explicit.
- **Cross-PR dependency gates** ("PR #N must merge first") — not modelled.
- **Pagination beyond 100** — treated as hard-block-then-skip, not auto-paginated. Auto-pagination doable later if active-PR overflow becomes common.

---

## Failure-mode budget

- **Poller crash mid-loop** (bash error, SIGKILL): caller (`git-janitor`) treats as code 3 (gh down) on the next invocation — re-run from poll 1. No persisted state.
- **Network partition mid-poll**: counted as one gh failure; 3 consecutive failures → code 3.
- **GitHub rate-limit (HTTP 429 / `rate limit exceeded`)**: counted as gh failure same as any other gh error; user sees the message and decides retry vs abandon.
- **Stale `$ORCH_USER` between sessions**: re-resolved at every session init; cached only within the session. Username change mid-session would require restart.
- **Race between `gh pr ready` and `gh api …/merge`**: GitHub serializes; if a new CI run kicks off, the merge call errors with "required check missing" → surfaced to user, no silent failure.
- **Worker process exits between gate-pass and merge call**: orchestrator restart re-polls from scratch (idempotent) before merging.

---

## Implementation log

- `c8c7fe06` · #298 (2026-05-19) — core landing: `poll_merge_gates` + `merge-gates.graphql` single-call gate, bats rig + fixtures, and AGENTS.md / git-janitor wiring. Matches this plan's § Files changed table.
- Hardening follow-ups (~15 PRs, 2026-05-19 → 05-30): #398, #420, #424, #426, #427, #428, #431, #475 (`MERGE_GATES_FLIP_READY` draft→ready + `gh_pr_ready_idempotent`), #481 (STALE recovery), #511, #554, #576 (`0352c96d`, cross-poll nudge/STALE-streak persistence — most recent code touch). Added `scripts/dev/merge-gates.sh` as the real entrypoint, `test-merge-gates.sh`, the `cr-out-of-band` / `tests-out-of-band` / `perf-out-of-band` label overrides, the CR=`NONE` auto-nudge, and the review-skipped (too-many-files) block.

## Deviations from plan

- **Shipped scope grew well beyond the plan.** `scripts/dev/merge-gates.sh` (not `merge-gates-prompt.sh`) is the operative entrypoint; the gate gained `MERGE_GATES_FLIP_READY`, the CR=`NONE` grace + auto-`@coderabbitai review` nudge, the review-skipped size block, STALE-resolved / streak handling, and per-PR label overrides — none in the original design.
- **25 fixtures shipped vs the 8 planned** (`tests/fixtures/merge_gates_*.json`), covering the added CR / STALE / size-skip / label paths.
- **Ongoing CR-gate hardening tracked separately** in `docs/plans/shipped/gate-enforcement-hardening.md` (agent→GitHub promotion), so this plan closes at the poller + bats level.

## Verification (actual)

- **Bucket A (bats):** `tests/bats/merge_gates.bats` green at the pre-push gate (`scripts/dev/test-all.sh`); the 30+ planned cases plus the added CR-nudge / STALE-streak / label-override cases all pass against the 25 fixtures.
- **Live:** exercised end-to-end across the ~15 follow-up PRs (real CI-red blocks, CR `CHANGES_REQUESTED` blocks, force-push STALE blocks, `SKIP_MERGE_GATES` bypass).
- **Build gate:** N/A — `scripts/` + `tests/bats/` only, no `Source/Core` C++.
