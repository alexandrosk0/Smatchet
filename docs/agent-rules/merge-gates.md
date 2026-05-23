# Merge gates

> Lifted from [`AGENTS.md`](../../AGENTS.md) § Merge gates per [`docs/design/agents-md-reduction.md`](../design/agents-md-reduction.md). AGENTS.md retains a load-bearing stub naming the three gates + the override / scope boundaries so external `AGENTS.md § <subsection>` references continue to resolve. Edit this file directly — no parallel copy in AGENTS.md.

Before the orchestrator, `git-janitor`, OR `smatchet-merge-watcher` (the host daemon per [`docs/design/smatchet-merge-watcher.md`](../design/smatchet-merge-watcher.md)) squash-merges a PR, it polls three conditions via one `gh api graphql` call (`scripts/dev/merge-gates.graphql`):

1. **CI** — every required check (`isRequired(pullRequestNumber: $pr) == true`) on `pullRequest.commits(last:1).commit.statusCheckRollup.contexts` must reach a passing terminal state.
   - **CheckRun**: pass = `status == "COMPLETED"` AND `conclusion in {SUCCESS, NEUTRAL, SKIPPED, STALE}`. Block = `conclusion in {FAILURE, TIMED_OUT, CANCELLED, ACTION_REQUIRED, STARTUP_FAILURE}`. Any non-COMPLETED status counts as pending.
   - **StatusContext**: default rule — any required context with `state != "SUCCESS"` blocks. `FAILURE` / `ERROR` fail; `PENDING` / `EXPECTED` pending.
   - Non-required checks ignored.
2. **CodeRabbit** — identity match is `author.login in {"coderabbitai", "coderabbitai[bot]"}` (REST returns the `[bot]` suffix; GraphQL may strip it). State combines the latest CR review's `state`, its `commit.oid` (vs `headRefOid`), and its `body` (CR's `Actionable comments posted: N` first-line header) into one of these outcomes:
   - `NONE` — no review ever submitted. The poller pre-detects whether CR is installed for this repo by probing for a checked-in `.coderabbit.yaml` / `.coderabbit.yml` (one-shot at gate start; override via `MERGE_GATES_CR_INSTALLED=true|false`). Behaviour splits:
     - **CR not installed** (no config file) → **pass** immediately (legacy behaviour for repos that never integrated CR).
     - **CR installed, head commit has a `CodeRabbit` StatusContext on the rollup with `state == "SUCCESS"`** → **pass** (status-only configs that skip writing a full review on clean diffs still count as a positive signal).
     - **CR installed, no review yet, no SUCCESS status** → **block** until the configurable grace window (`MERGE_GATES_CR_GRACE_POLLS`, default 10 polls) expires. After the window, the poller logs a `WARN: CodeRabbit grace window ... expired` line and falls through to pass so a stuck integration never wedges the ship-loop indefinitely.
   - **On-head review** (`commit.oid == headRefOid`):
     - `APPROVED` → **pass** unconditionally (approval trumps body).
     - `COMMENTED + body has "Actionable comments posted: N" with N > 0` → **block** (CR found real findings the user must address before merge). The previous "COMMENTED == pass" rule shipped 5 unaddressed CR findings to develop on PR #357 — see `docs/backlog/agent-self-improvement/process.md` P1 (2026-05-21) for the post-mortem.
     - `COMMENTED + N == 0` → **pass** (CR explicitly said no findings).
     - `COMMENTED + no Actionable header in body` → **pass** (placeholder review / older CR template; conservative).
     - `CHANGES_REQUESTED` / `DISMISSED` → **block**.
   - **Stale review** (CR reviewed a prior commit; force-push or post-review commit moved HEAD past the review):
     - `STALE_WITH_FINDINGS` (prior body had `N > 0`) → **block** + DO NOT fall through to pass on timeout. The timeout-fallthrough path on stale-with-findings is what dropped #357's 5 findings; the prior review body must be surfaced + the user explicitly authorises any force-merge.
     - `STALE_RESOLVED` (prior body had `N > 0` BUT every CR review thread is resolved AND the on-head `CodeRabbit` StatusContext is `SUCCESS`) → **pass**. CR's accept signal for a fixup commit is *thread resolution* on re-review; it does not always re-issue a clean "Actionable comments posted: 0" review body. Both signals are required together — `cr_open == 0` alone could be user-driven thread resolution without CR judgement; `StatusContext == SUCCESS` alone could be a stale placeholder. Together they unambiguously mean "CR re-evaluated the current head and accepted the fix." Codified per the recurring stuck-queue pattern hit on PRs #421/#422/#423/#425 on 2026-05-23.
     - `STALE_CLEAN` (prior body had `N == 0`) → **pass** (prior review was clean; on-head changes likely still clean modulo new edits).
     - `STALE_UNKNOWN` (prior body absent or no `Actionable` header) → **block** as safe default (caller can't distinguish "0 actionable" from "no header", so the safer assumption is "could have findings").
   - Additionally: zero unresolved non-outdated review threads contain a CodeRabbit comment (under the same login match).
3. **User comments** — zero unresolved non-outdated review threads with any non-bot non-self comment, AND zero conversation-tab comments from a non-bot non-self author. Bot detection uses GraphQL `author.__typename == "Bot"` (covers all integrations). Self matched via `$ORCH_USER`, lower-cased on both sides.

Additional pass conditions:
- `pullRequest.state == "OPEN"` (early-exit on closed/merged-externally).
- `pullRequest.reviewDecision in {"APPROVED", null}` (blocks on `REVIEW_REQUIRED` / `CHANGES_REQUESTED`).
- **Pagination ceiling**: GitHub GraphQL caps connections at 100. The query also fetches `pageInfo.hasNextPage` for every connection (checks, reviews, reviewThreads, per-thread comments, conversation comments). Any `hasNextPage == true` → block with `PAGINATION_OVERFLOW`. Hard block, not silent truncation.

`$ORCH_USER` resolved at session init via `gh api user --jq .login`.

**Override**: `SKIP_MERGE_GATES=true` at session init bypasses all gates. No per-merge skip. Subagent propagation: orchestrator must explicitly add `SKIP_MERGE_GATES` to any delegated `git-janitor` invocation's env (it does not auto-inherit through the subagent boundary).

**Per-PR overrides (label-based)**:
- `tests-out-of-band` — downgrades the test-delta gate from FAIL to WARN for that PR. Use when production code changes legitimately have no testable surface (e.g. perf optimisations that preserve behaviour but lack pure-logic seams).
- `perf-out-of-band` — downgrades the `.github/workflows/perf-pr-fast.yml` regression gate (slice 3 of `docs/design/pillar-1-2-perf-review-system.md`) from FAIL to WARN. Use when a regression is intentional + the baseline-bump PR is queued. The label must NOT stay on the PR post-merge; the merge contract assumes the next PR clears the regression or bumps the baseline.

**Status line per poll**:
```
Poll 3/60 — CI: 4/8 pass (1 fail, 2 pending, 1 warn-downgraded) | CodeRabbit: COMMENTED (3 actionable — block) (2 open) | User: 1 | reviewDecision: APPROVED
```

The `warn-downgraded` cell counts CheckRuns whose failing conclusion (`FAILURE`, `TIMED_OUT`, `CANCELLED`, `ACTION_REQUIRED`, `STARTUP_FAILURE`) was suppressed by a per-PR label override (`tests-out-of-band` / `perf-out-of-band`). Downgraded checks do NOT contribute to `fail` and do NOT block; an `WARN: out-of-band label(s) downgraded …` line on stderr names which checks were silenced for the operator's review.

The CR cell encodes the outcomes verbatim — examples: `APPROVED`, `COMMENTED (3 actionable — block)`, `COMMENTED (0 actionable)`, `COMMENTED (no Actionable header)`, `STALE_WITH_FINDINGS (5 actionable on prior commit — block + surface review)`, `STALE_RESOLVED (4 actionable on prior commit, all threads resolved + status SUCCESS — pass)`, `STALE_CLEAN (0 actionable on prior commit — pass)`, `STALE_UNKNOWN (no Actionable header — treat as block per safe-default policy)`, `CHANGES_REQUESTED`, `DISMISSED`, `NONE+pending (poll N/<grace>)`, `NONE+status-SUCCESS`, `NONE+grace-expired` (paired with `WARN` on stderr).

**Halt prompts (per return code)**:

| Code | Meaning | `AskUserQuestion` options |
|---|---|---|
| 1 | Gates still blocked at MAX_POLLS | "Skip gates and merge anyway" / "Keep waiting (extend poll)" / "Abandon" |
| 2 | Wall-clock timeout (≥`MERGE_GATES_TIMEOUT_SECONDS`) | "Skip gates and merge anyway" / "Keep waiting" / "Abandon" |
| 3 | `gh` API failed 3 consecutive polls | "Retry now" / "Skip gates and merge anyway" / "Abandon" |
| 4 | PR `CLOSED` or `MERGED` externally | "Abandon (PR no longer mergeable)" — no skip option |
| 5 | Pagination overflow (any `hasNextPage`) | "Abandon (manual review required)" / "Skip gates and merge anyway (acknowledge risk)" |
| 6 | `gh pr ready` unknown failure (after positive-check fallback) | surface error to user; do not auto-merge |

Any "Skip gates and merge anyway" choice logs `LOG_WARN "user skipped gates: code=<n>"` before proceeding.

**Auto-`gh pr ready` + merge** apply only when the user has explicitly authorised this PR for merge (post-ship option 3 "Register with watcher", in-session "merge when green", OR any PR registered with `smatchet-merge-watcher`). Without that authorization, gate-pass is reported and the orchestrator stops without flipping draft state. Use REST merge per `agents/git-janitor.md` § Hard refusals:

```bash
gh api -X PUT "repos/$owner/$repo/pulls/$prNumber/merge" -f merge_method=squash
gh api -X DELETE "repos/$owner/$repo/git/refs/heads/$branch"
```

Conflicts, missing required checks, and branch-protection rules are enforced by GitHub on the REST merge call. We do not duplicate.

**Env knobs**:
- `MERGE_GATES_POLL_INTERVAL` — seconds between polls (default 60).
- `MERGE_GATES_MAX_POLLS` — max poll count (default 60).
- `MERGE_GATES_TIMEOUT_SECONDS` — wall-clock budget (default 3600).
- `MERGE_GATES_QUERY_FILE` — override GraphQL document path (default `scripts/dev/merge-gates.graphql`).
- `MERGE_GATES_CR_INSTALLED` — override the auto-detected CodeRabbit-installed flag (`true` / `false`). Auto-detection probes `repos/<owner>/<repo>/contents/.coderabbit.yaml` (and `.yml`); set explicitly when the config lives outside the repo or when running against a fork that has not yet enabled CR.
- `MERGE_GATES_CR_GRACE_POLLS` — polls to wait for CR to start (a review or `CodeRabbit` SUCCESS status) before falling through `NONE` to pass (default 10). Only consulted when `MERGE_GATES_CR_INSTALLED` is true / auto-detected as installed.
- `MERGE_GATES_TEST_ANSWER` — bats-only canned `ask_user_question` answer.

**Scope boundary**: the auto-`gh pr ready` + auto-merge path applies to the orchestrator, `git-janitor`, and `smatchet-merge-watcher`. No other caller has merge authority. The deleted spawned-child agents (`handoff-implementer`, `pr-iterator`) are gone per v1 of `docs/design/github-tracker-backend.md`; the watcher runs as a host daemon, not a per-PR subprocess, so the spawned-child draft-only carve-out no longer applies.

Implementation: `scripts/dev/merge-gates.sh` (sourceable + CLI), `scripts/dev/merge-gates-prompt.sh` (`ask_user_question` shim), `scripts/dev/merge-gates.graphql`. Tests: `tests/bats/merge_gates.bats` + `tests/fixtures/merge_gates_*.json`.
