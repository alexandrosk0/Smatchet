# Plan: Merge Gates — CI green + CodeRabbit + user comments before merge

## Context

Current ship-loop: `diagnose → fix → build → commit → push → open PR → squash-merge → cleanup`. No CI wait, no comment-resolution gate. Post-ship offers 4 options: Manual verify / Review PR / Squash-merge / Done.

Add a gate-check step between "open PR" and "squash-merge" that polls three conditions via a single GraphQL query, blocks the merge until all clear (or user overrides). Bash-only runtime; tests via `bats`. No C++ surface.

**Environment**:
- `SKIP_MERGE_GATES=true` — bypass all gates (session-init override).

---

## 1. AGENTS.md — ship-loop sequence

Replace ship-loop (line 77):
```
BEFORE: diagnose → fix → build → commit → push → open PR → squash-merge → cleanup
AFTER:  diagnose → fix → build → commit → push → open PR → [gate-check] → squash-merge → cleanup
```

`[gate-check]` polls every 60s, 60min cap. Auto-merge on pass. Halt + ask user on block / timeout / gh failure.

---

## 2. AGENTS.md — merge gates section (new)

```
### Merge gates

Before squash-merge, orchestrator polls three conditions via one GraphQL query
(`gh api graphql`):

1. **CI**: every required check (`isRequired(pullRequestNumber: $pr) == true`)
   in `pullRequest.commits(last:1).commit.statusCheckRollup.contexts` has
   `conclusion == SUCCESS` (CheckRun) or `state == SUCCESS` (StatusContext).
   Skipped/neutral ignored. Pending blocks.

2. **CodeRabbit**: latest review by `coderabbitai` **submitted against the
   current `headRefOid`** is NOT `CHANGES_REQUESTED`, AND zero unresolved
   non-outdated review threads contain a `coderabbitai` comment. Reviews on
   pre-force-push SHAs are ignored (force-push race protection).

3. **User comments**: zero unresolved non-outdated review threads with any
   non-bot non-self comment, AND zero conversation-tab comments from a
   non-bot non-self author. Bot detection uses GraphQL
   `author.__typename == "Bot"` (covers all integrations, not just CodeRabbit).

Additional pass conditions:
- `pullRequest.state == "OPEN"` (early-exit on closed/merged-externally).
- `pullRequest.reviewDecision in {"APPROVED", null}` (blocks on REVIEW_REQUIRED / CHANGES_REQUESTED).
  - Note: when repo has branch protection requiring approvals, GitHub also enforces this server-side on the merge call. Our gate is meaningful when branch protection is absent but reviewers were requested.

Conflict and missing-required-check enforcement is delegated to GitHub: `gh pr merge` errors on either; we surface the error to user. No client-side duplicate.

`$ORCH_USER` resolved at session init via `gh api user --jq .login`.

Override: `SKIP_MERGE_GATES=true` at session init bypasses all gates. No
per-merge skip.

Status line per poll:
  "Poll 3/60 — CI: 5/8 pass (1 fail, 2 pending) | CodeRabbit: CHANGES_REQUESTED (2 threads) | User: 1"

Auto-convert draft → ready (`gh pr ready`) when gates pass.
```

---

## 3. AGENTS.md — post-ship protocol

Replace current 4 AskUserQuestion options — option 3 swaps "Squash-merge" → "Wait for gates":

1. **Manual verify** (unchanged)
2. **Review PR** (unchanged)
3. **Wait for gates** — orchestrator polls every 60s. Auto-merge on pass. Timeout → AskUserQuestion: "Skip gates and merge" / "Abandon".
4. **Done** — PR stays draft (unchanged)

Skip-condition: user said "merge when green" → enter option 3 implicitly.

---

## 4. git-janitor.md — gate check before merge

**Current flow**: verify mergeable → squash-merge → delete branch → plan revision

**New flow**: verify mergeable → poll_merge_gates → `gh pr ready` (if draft) → squash-merge → delete branch → plan revision

### GraphQL query — `scripts/dev/merge-gates.graphql`

```graphql
query($owner: String!, $repo: String!, $pr: Int!) {
  repository(owner: $owner, name: $repo) {
    pullRequest(number: $pr) {
      state                  # OPEN / CLOSED / MERGED — exit poller when not OPEN
      isDraft
      reviewDecision         # APPROVED / REVIEW_REQUIRED / CHANGES_REQUESTED / null
      headRefOid
      commits(last: 1) {
        nodes { commit { statusCheckRollup { contexts(first: 100) { nodes {
          __typename
          ... on CheckRun     { name conclusion status isRequired(pullRequestNumber: $pr) }
          ... on StatusContext { context state isRequired(pullRequestNumber: $pr) }
        }}}}}
      }
      reviews(first: 50) {
        nodes {
          author { login __typename }
          state submittedAt
          commit { oid }     # force-push race: ignore reviews not on headRefOid
        }
      }
      reviewThreads(first: 100) {
        nodes {
          isResolved isOutdated
          comments(first: 5) { nodes { author { login __typename } } }
        }
      }
      comments(first: 100) {
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
    local POLL_INTERVAL=60 MAX_POLLS=60
    local start=$(date +%s) gh_fails=0
    local q="scripts/dev/merge-gates.graphql"

    for ((p=0; p<MAX_POLLS; p++)); do
        local data
        if ! data=$(gh api graphql -f owner="$owner" -f repo="$repo" \
                       -F pr="$prNumber" -F query=@"$q" 2>&1); then
            gh_fails=$((gh_fails+1))
            echo "Poll $((p+1)): gh failed ($gh_fails/3): $data"
            [ "$gh_fails" -ge 3 ] && { echo "GH_API_DOWN"; return 3; }
            sleep "$POLL_INTERVAL"; continue
        fi
        gh_fails=0
        local pr=$(jq '.data.repository.pullRequest' <<<"$data")

        # PR state early-exit (closed/merged externally)
        local pr_state=$(jq -r '.state' <<<"$pr")
        if [ "$pr_state" != "OPEN" ]; then
            echo "PR_$pr_state"; return 4
        fi
        local head_sha=$(jq -r '.headRefOid' <<<"$pr")

        # CI
        local ctx='.commits.nodes[0].commit.statusCheckRollup.contexts.nodes // []'
        local ci_total=$(jq "[$ctx | .[] | select(.isRequired==true)] | length" <<<"$pr")
        local ci_fail=$(jq "[$ctx | .[] | select(.isRequired==true) | select(
            (.__typename==\"CheckRun\"      and .status==\"COMPLETED\" and ((.conclusion // \"\") | IN(\"FAILURE\",\"TIMED_OUT\",\"CANCELLED\",\"ACTION_REQUIRED\",\"STARTUP_FAILURE\"))) or
            (.__typename==\"StatusContext\" and .state==\"FAILURE\")
        )] | length" <<<"$pr")
        local ci_pend=$(jq "[$ctx | .[] | select(.isRequired==true) | select(
            (.__typename==\"CheckRun\"      and .status!=\"COMPLETED\") or
            (.__typename==\"StatusContext\" and .state==\"PENDING\")
        )] | length" <<<"$pr")

        # CodeRabbit — only reviews on current head (force-push race protection)
        local cr_state=$(jq -r --arg sha "$head_sha" '
            [.reviews.nodes[]
             | select(.author.login=="coderabbitai")
             | select(.commit.oid==$sha)]
            | sort_by(.submittedAt) | (.[-1].state // "NONE")' <<<"$pr")
        local cr_open=$(jq '[.reviewThreads.nodes[]
            | select(.isResolved==false and .isOutdated==false
                     and any(.comments.nodes[]; .author.login=="coderabbitai"))] | length' <<<"$pr")

        # User comments — typename-based bot filter, both surfaces
        local user=$(jq --arg self "$ORCH_USER" '
            ([.comments.nodes[]
              | select(.author.__typename != "Bot" and .author.login != $self)] | length) +
            ([.reviewThreads.nodes[]
              | select(.isResolved==false and .isOutdated==false
                       and any(.comments.nodes[];
                               .author.__typename != "Bot" and .author.login != $self))] | length)
        ' <<<"$pr")

        # reviewDecision — relevant when branch protection absent
        local review_decision=$(jq -r '.reviewDecision // "NONE"' <<<"$pr")
        local review_pass=false
        case "$review_decision" in
            APPROVED|NONE) review_pass=true ;;
        esac

        echo "Poll $((p+1))/$MAX_POLLS — CI: $((ci_total-ci_fail-ci_pend))/$ci_total pass ($ci_fail fail, $ci_pend pending) | CodeRabbit: $cr_state ($cr_open open) | User: $user | reviewDecision: $review_decision"

        local cr_pass=false
        case "$cr_state" in APPROVED|COMMENTED|NONE) cr_pass=true ;; esac

        if [ "$ci_fail" -eq 0 ] && [ "$ci_pend" -eq 0 ] && \
           [ "$cr_pass" = true ] && [ "$cr_open" -eq 0 ] && \
           [ "$user" -eq 0 ] && [ "$review_pass" = true ]; then
            echo "GATES_PASSED"; return 0
        fi

        [ $(( $(date +%s) - start )) -ge 3600 ] && { echo "GATES_TIMEOUT"; return 2; }
        [ "$p" -lt $((MAX_POLLS-1)) ] && sleep "$POLL_INTERVAL"
    done
    return 1
}
```

**Return codes**:
- 0 — gates passed
- 1 — gates still blocked at MAX_POLLS
- 2 — timeout (60min wall-clock exceeded)
- 3 — gh API down (3 consecutive failures)
- 4 — PR closed or merged externally

Conflicts and missing-required-checks are not separate return codes — `gh pr merge` errors on them; the caller surfaces the error.

### Integration in git-janitor

```
1. Session init (once):
   - gh auth status || FAIL_LOUD
   - ORCH_USER=$(gh api user --jq .login)
2. verify_mergeable
3. [ "$SKIP_MERGE_GATES" = true ] && goto step 5
4. poll_merge_gates
   - 0: continue
   - 1 (blocked) / 2 (timeout) / 3 (gh down) / 4 (PR closed): halt
     + AskUserQuestion "Skip gates and merge" / "Abandon"
     + If user picks "Skip gates and merge": LOG_WARN "user skipped gates: reason=<code>"
5. If isDraft from last poll: `gh pr ready $prNumber 2>/dev/null || true`  # idempotent
6. `gh pr merge $prNumber --squash --delete-branch`
   - If GitHub-side enforcement blocks (conflicts, missing required checks,
     unmet branch protection): surface gh error to user, halt.
7. plan revision
```

**Notes**:
- CodeRabbit state `NONE` (no review yet on current head) passes the gate. Users either wait manually before triggering merge, or set `SKIP_MERGE_GATES=true` if their repo lacks CodeRabbit.
- No hard-fail short-circuit. If CI definitively failed, user sees it on next 60s poll and picks "Skip / Abandon".
- Force-push race handled by `review.commit.oid == headRefOid` filter.
- Bot filter uses `author.__typename != "Bot"` — covers all GitHub bot integrations without enumerating logins.
- Conflicts, missing required checks, and branch-protection rules are enforced by GitHub on the merge call. We do not duplicate.

---

## Files changed

| File | Change |
|---|---|
| `AGENTS.md` | Ship-loop add `[gate-check]`; new Merge-gates section; post-ship option 3 swap |
| `agents/git-janitor.md` | Insert poller flow before squash-merge; session-init prerequisites; AskUserQuestion on halt |
| `scripts/dev/merge-gates.graphql` | GraphQL document |
| `tests/bats/merge_gates.bats` | Drive `poll_merge_gates` against fixture JSONs |
| `tests/fixtures/merge_gates_*.json` | 5 fixtures (see Verification) |

---

## Verification

`bats` is the runtime test harness. Stub `gh` via `PATH` shim — fixture-driven.

### Fixtures (6 files in `tests/fixtures/`)

1. `merge_gates_pass.json` — all gates clear; includes skipped/neutral CheckRun + outdated threads + bot comments + self comment (all should NOT block)
2. `merge_gates_ci_fail.json` — required CheckRun with conclusion FAILURE
3. `merge_gates_ci_pending.json` — required CheckRun status IN_PROGRESS
4. `merge_gates_cr_changes.json` — coderabbitai latest review CHANGES_REQUESTED + 2 unresolved threads
5. `merge_gates_cr_stale.json` — coderabbitai APPROVED but on old SHA (force-push race); should NOT pass
6. `merge_gates_state.json` — covers `state=CLOSED` and `reviewDecision=REVIEW_REQUIRED` (tests select with jq overlays)

### Test cases in `merge_gates.bats`

```bash
# Pass / block by gate
@test "all gates pass → return 0"                                 # fixture 1
@test "CI failure → return 1 with status"                         # fixture 2
@test "CI pending → return 1 with status"                         # fixture 3
@test "CodeRabbit CHANGES_REQUESTED → return 1"                   # fixture 4
@test "user comment present → return 1"                           # fixture 4 (variant)

# Early-exit + reviewDecision
@test "PR state=CLOSED → return 4"                                # fixture 6
@test "PR state=MERGED → return 4"                                # fixture 6
@test "reviewDecision=REVIEW_REQUIRED → return 1"                 # fixture 6
@test "reviewDecision=APPROVED → contributes to pass"             # fixture 1

# Race & filter edge cases
@test "stale CodeRabbit APPROVED on old SHA → not counted"        # fixture 5
@test "Bot-typename comment does not block"                       # fixture 1
@test "outdated thread does not block"                            # fixture 1
@test "self comment does not block"                               # fixture 1
@test "skipped/neutral CheckRun does not block"                   # fixture 1
@test "no CodeRabbit review treated as pass"                      # fixture 1 (empty reviews)

# Operational
@test "gh pr ready when already ready does not crash"             # mock gh exit nonzero
@test "SKIP_MERGE_GATES=true bypasses poller"                     # git-janitor flow

# Infra
@test "3 consecutive gh failures → return 3"                      # mock gh exit 1
@test "synthetic 3601s elapsed → return 2"                        # mock date
@test "user-skip path emits LOG_WARN"                             # mock AskUserQuestion → skip
```

### Manual smoke

1. Open a real PR with red CI → trigger gate-check → expect block + status
2. Resolve all gates → expect pass + auto-merge
3. `SKIP_MERGE_GATES=true` → expect immediate merge without polling

---

## Implementation log

(filled in after PR merges)

## Deviations from plan

(filled in after PR merges)
