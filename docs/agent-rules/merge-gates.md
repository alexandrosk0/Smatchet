# Merge gates

> Lifted from [`AGENTS.md`](../../AGENTS.md) § Merge gates per [`docs/plans/shipped/agents-md-reduction.md`](../plans/shipped/agents-md-reduction.md). AGENTS.md retains a load-bearing stub naming the three gates + the override / scope boundaries so external `AGENTS.md § <subsection>` references continue to resolve. Edit this file directly — no parallel copy in AGENTS.md.

Before the orchestrator, `git-janitor`, OR `smatchet-merge-watcher` (the host daemon per [`docs/plans/shipped/smatchet-merge-watcher.md`](../plans/shipped/smatchet-merge-watcher.md)) squash-merges a PR, it polls three conditions via one `gh api graphql` call (`agents/scripts/core/merge-gates.graphql`):

1. **CI** — every required check (`isRequired(pullRequestNumber: $pr) == true`) on `pullRequest.commits(last:1).commit.statusCheckRollup.contexts` must reach a passing terminal state.
   - **CheckRun**: pass = `status == "COMPLETED"` AND `conclusion in {SUCCESS, NEUTRAL, SKIPPED, STALE}`. Block = `conclusion in {FAILURE, TIMED_OUT, CANCELLED, ACTION_REQUIRED, STARTUP_FAILURE}`. Any non-COMPLETED status counts as pending.
   - **StatusContext**: default rule — any required context with `state != "SUCCESS"` blocks. `FAILURE` / `ERROR` fail; `PENDING` / `EXPECTED` pending.
   - Non-required checks ignored.
2. **CodeRabbit** — identity match is `author.login in {"coderabbitai", "coderabbitai[bot]"}` (REST returns the `[bot]` suffix; GraphQL may strip it). State combines the latest CR review's `state`, its `commit.oid` (vs `headRefOid`), and its `body` (CR's `Actionable comments posted: N` first-line header) into one of these outcomes:
   - `NONE` — no review ever submitted. The poller pre-detects whether CR is installed for this repo by probing for a checked-in `.coderabbit.yaml` / `.coderabbit.yml` (one-shot at gate start; override via `MERGE_GATES_CR_INSTALLED=true|false`). Behaviour splits:
     - **CR not installed** (no config file) → **pass** immediately (legacy behaviour for repos that never integrated CR).
     - **CR installed, StatusContext SUCCESS + at least one CR review-thread comment on the current head** → **pass** (C4 prong 2 — the placeholder status is corroborated by real CR review activity on this commit; the inline comment proves CR actually reviewed the current head).
     - **CR installed, StatusContext SUCCESS but ZERO inline CR comments on `headRefOid`** → **block within the grace window**; after `MERGE_GATES_CR_GRACE_POLLS` (default 10) expires, fall through to pass with a `WARN: ... status=SUCCESS but no inline CR comments on head after grace ...; possible status-only config OR C4 bypass` line. The grace-then-pass keeps status-only CR integrations (no inline reviews ever emitted) viable, while the WARN names the suspicious shape so the operator can catch a real C4 bypass case where CR's `auto_review.drafts:false` skipped the review but the placeholder StatusContext still fired SUCCESS.
     - **CR installed, no review yet, no SUCCESS status** → **block** until the grace window expires. After the window, the poller logs a `WARN: CodeRabbit grace window ... expired` line and falls through to pass so a stuck integration never wedges the ship-loop indefinitely.
     - **CR=NONE early-nudge** (default **OFF** — `reduce-coderabbit-review-spend` Slice 2): when `MERGE_GATES_NONE_NUDGE_POLLS` > 0, after that many consecutive blocking `NONE` polls on a head (CR installed, `cr_pass=false`, within grace, no CR context yet) the poller posts `@coderabbitai review` once per HEAD. **Default `0` = disabled**, because `.coderabbit.yaml` `auto_review` already reviews every push, so the nudge was redundant and — when it fired on the *first* NONE poll — raced ahead of auto_review (the per-PR redundant `@coderabbitai review` the last-10-PRs audit measured). Shares the once-per-HEAD guard with the STALE re-review trigger (never double-posts). The grace-then-pass backstop above + the STALE trigger remain the safety nets, so disabling the NONE nudge loses no review coverage.
     - **CR review-skipped (too many files)** → **block** unless `cr-out-of-band` is applied. When a PR exceeds CodeRabbit's configured file limit, CR does *not* submit a review object — it posts an auto-generated PR **conversation (issue) comment** whose body carries the stable HTML marker `<!-- ... skip review by coderabbit.ai -->` (fallback text: `Review skipped` + `Too many files`). Because there's no review object, `reviewDecision` stays `NONE` and `cr_state` computes to `NONE`, so the passive NONE grace-then-pass backstop would otherwise wave a completely-unreviewed PR straight through (this is exactly the hole that let a 638-file reorg merge with zero CR review). The poller detects the marker in any `coderabbitai` / `coderabbitai[bot]`-authored conversation comment and treats it as a **hard block** that overrides the grace counter — printed as `NONE+size-skip (CR skipped review — too many files)`, with `BLOCK: CodeRabbit skipped review — too many files (exceeds CR file limit); split the PR (coderabbit review --dir <path> / --base) or apply the 'cr-out-of-band' label to merge without CR review.` on stderr. The futile `@coderabbitai review` early-nudge is **suppressed** for this case (re-triggering review on an over-limit PR just makes CR skip again + spams the thread). The detection is consulted **only when `cr_state == NONE`** (no review object on any commit), so if the PR is later reduced/split and CR posts a real on-head review, the most-recent CR signal (`APPROVED` / `COMMENTED` / `CHANGES_REQUESTED` / `STALE*`) takes precedence and the stale skip comment is ignored. Override: `cr-out-of-band` downgrades it to a pass with `WARN: cr-out-of-band — CR skipped review (too many files) overridden`.
   - **On-head review** (`commit.oid == headRefOid`):
     - `APPROVED` → **pass** unconditionally (approval trumps body).
     - `COMMENTED + body has "Actionable comments posted: N" with N > 0` → **block** (CR found real findings the user must address before merge). The previous "COMMENTED == pass" rule shipped 5 unaddressed CR findings to develop on PR #357 — see `docs/self-improvement/categories/process.md` P1 (2026-05-21) for the post-mortem.
     - `COMMENTED + N == 0` → **pass** (CR explicitly said no findings).
     - `COMMENTED + no Actionable header in body` → **pass** (placeholder review / older CR template; conservative).
     - `CHANGES_REQUESTED` / `DISMISSED` → **block**.
   - **Stale review** (CR reviewed a prior commit; force-push or post-review commit moved HEAD past the review):
     - `STALE_WITH_FINDINGS` (prior body had `N > 0`) → **block** + DO NOT fall through to pass on timeout. The timeout-fallthrough path on stale-with-findings is what dropped #357's 5 findings; the prior review body must be surfaced + the user explicitly authorises any force-merge.
     - `STALE_RESOLVED` (prior body had `N > 0` BUT every CR review thread is resolved AND the on-head `CodeRabbit` StatusContext is `SUCCESS`) → **pass**. CR's accept signal for a fixup commit is *thread resolution* on re-review; it does not always re-issue a clean "Actionable comments posted: 0" review body. Both signals are required together — `cr_open == 0` alone could be user-driven thread resolution without CR judgement; `StatusContext == SUCCESS` alone could be a stale placeholder. Together they unambiguously mean "CR re-evaluated the current head and accepted the fix." Codified per the recurring stuck-queue pattern hit on PRs #421/#422/#423/#425 on 2026-05-23.
     - `STALE_CLEAN` (prior body had `N == 0`) → **pass** (prior review was clean; on-head changes likely still clean modulo new edits).
     - `STALE_UNKNOWN` (prior body absent or no `Actionable` header) → **block** as safe default (caller can't distinguish "0 actionable" from "no header", so the safer assumption is "could have findings").
   - Additionally: zero unresolved non-outdated review threads contain a CodeRabbit comment (under the same login match). CR sometimes leaves per-line review threads `isResolved:false` on prior commits even after its overall review on the new head is SUCCESS, which kept `cr_open > 0` and wedged merges historically (2026-05-22 PRs #408/#410). `smatchet-merge-watcher` exposes a default-on side-channel — `MERGE_WATCH_RESOLVE_CR_THREADS` (default `true` as of 2026-05-28; set `false` / `0` / `no` to opt out) — that calls GraphQL `mutation resolveReviewThread` per stuck CR-authored thread after an auto-act push lands on a new head (gate logic detailed in `docs/plans/shipped/smatchet-merge-watcher.md` § decision 7). Manual unblock: `gh api graphql -f query='mutation($id:ID!){resolveReviewThread(input:{threadId:$id}){thread{id}}}' -F id=<thread-id>`.
3. **User comments** — zero unresolved non-outdated review threads with any non-bot non-self comment, AND zero conversation-tab comments from a non-bot non-self author. Bot detection uses GraphQL `author.__typename == "Bot"` (covers all integrations). Self matched via `$ORCH_USER`, lower-cased on both sides.

Additional pass conditions:
- `pullRequest.state == "OPEN"` (early-exit on closed/merged-externally).
- `pullRequest.reviewDecision in {"APPROVED", null}` (blocks on `REVIEW_REQUIRED` / `CHANGES_REQUESTED`).
- **Pagination ceiling**: GitHub GraphQL caps connections at 100. The query also fetches `pageInfo.hasNextPage` for every connection (checks, reviews, reviewThreads, per-thread comments, conversation comments). Any `hasNextPage == true` → block with `PAGINATION_OVERFLOW`. Hard block, not silent truncation.

`$ORCH_USER` resolved at session init via `gh api user --jq .login`.

**Override**: `SKIP_MERGE_GATES=true` at session init bypasses all gates. No per-merge skip. Subagent propagation: orchestrator must explicitly add `SKIP_MERGE_GATES` to any delegated `git-janitor` invocation's env (it does not auto-inherit through the subagent boundary).

**Per-PR overrides (label-based)**:
- `tests-out-of-band` — downgrades the test-delta gate from FAIL to WARN for that PR. Use when production code changes legitimately have no testable surface (e.g. perf optimisations that preserve behaviour but lack pure-logic seams).
- `perf-out-of-band` — downgrades the `.github/workflows/perf-pr-fast.yml` regression gate (slice 3 of `docs/plans/shipped/pillar-1-2-perf-review-system.md`) from FAIL to WARN. Use when a regression is intentional + the baseline-bump PR is queued. The label must NOT stay on the PR post-merge; the merge contract assumes the next PR clears the regression or bumps the baseline.
- `cr-out-of-band` — downgrades a **CodeRabbit** block to WARN (pass). Scoped to the CR gate ONLY: it waives the CR state verdict (`CHANGES_REQUESTED`, `COMMENTED + N > 0`, `DISMISSED`, `STALE_WITH_FINDINGS`, `STALE_UNKNOWN`, a still-blocking `NONE`, or a **review-skipped — too many files** comment), and any unresolved CodeRabbit-authored review threads (`cr_open`). It does NOT touch the CI gate (`ci_fail` / `ci_pend`) or the user-comment gate — a failing required check or an unresolved human comment still blocks. Emits `WARN: cr-out-of-band label downgraded CR block (<state>) to WARN` on stderr (the review-skipped case emits the tailored `WARN: cr-out-of-band — CR skipped review (too many files) overridden` instead). Use when CR's findings are knowingly acceptable / out-of-band (e.g. a follow-up PR addresses them) or when an over-limit PR legitimately can't be split. Like the other out-of-band labels, it MUST NOT stay on the PR post-merge.

**Status line per poll**:
```
Poll 3/60 — CI: 4/8 pass (1 fail, 2 pending, 1 warn-downgraded) | CodeRabbit: COMMENTED (3 actionable — block) (2 open) | User: 1 | reviewDecision: APPROVED
```

The `warn-downgraded` cell counts CheckRuns whose failing conclusion (`FAILURE`, `TIMED_OUT`, `CANCELLED`, `ACTION_REQUIRED`, `STARTUP_FAILURE`) was suppressed by a per-PR label override (`tests-out-of-band` / `perf-out-of-band`). Downgraded checks do NOT contribute to `fail` and do NOT block; an `WARN: out-of-band label(s) downgraded …` line on stderr names which checks were silenced for the operator's review.

The CR cell encodes the outcomes verbatim — examples: `APPROVED`, `COMMENTED (3 actionable — block)`, `COMMENTED (0 actionable)`, `COMMENTED (no Actionable header)`, `STALE_WITH_FINDINGS (5 actionable on prior commit — block + surface review)`, `STALE_RESOLVED (4 actionable on prior commit, all threads resolved + status SUCCESS — pass)`, `STALE_CLEAN (0 actionable on prior commit — pass)`, `STALE_UNKNOWN (no Actionable header — treat as block per safe-default policy)`, `CHANGES_REQUESTED`, `DISMISSED`, `NONE+pending (poll N/<grace>)`, `NONE+status-SUCCESS+inline-evidence`, `NONE+status-SUCCESS-waiting-for-inline (poll N/<grace>)`, `NONE+status-SUCCESS+no-inline-evidence`, `NONE+grace-expired`, `NONE+size-skip (CR skipped review — too many files)` (all `WARN`/`BLOCK`-paired on stderr where noted).

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

**Auto-`gh pr ready` + merge** apply only when the user has explicitly authorised this PR for merge (post-ship option 3 "Register with watcher", in-session "merge when green", OR any PR registered with `smatchet-merge-watcher`). Without that authorization, gate-pass is reported and the orchestrator stops without flipping draft state. Use REST merge per `agents/core/git-janitor.md` § Hard refusals:

```bash
gh api -X PUT "repos/$owner/$repo/pulls/$prNumber/merge" -f merge_method=squash
gh api -X DELETE "repos/$owner/$repo/git/refs/heads/$branch"
```

Conflicts, missing required checks, and branch-protection rules are enforced by GitHub on the REST merge call. We do not duplicate.

**GitHub merge queue**: a repo may put its protected branch behind a GitHub merge queue, in which case the branch-protection required checks gate the merge by re-running on the queue's synthetic `merge_group` ref. Every workflow hosting a required check must then also trigger on `merge_group`, and every required job must either run there or self-gate to a green no-op that reports the same check context — a required check that never reports on `merge_group` **deadlocks the queue forever** (the same path-filtered-required-check class that previously deadlocked product-only PRs). When a queue is active a **direct** REST merge (`PUT pulls/{pr}/merge`) returns 405; the queue-safe path is to **enable auto-merge** (`gh pr merge --squash --auto`), which merges immediately if no queue is set and enqueues if one is — the merge-watcher's PASS handler uses `--auto` accordingly, keeping its `merge-gates.sh` gate-poll as the pre-enqueue check. The custom poller above + the `*-out-of-band` labels remain for the legacy / manual (non-queue) path; **CodeRabbit stays PR-advisory** — not a queue-required `merge_group` check.

**Env knobs**:
- `MERGE_GATES_POLL_INTERVAL` — seconds between polls (default 60).
- `MERGE_GATES_MAX_POLLS` — max poll count (default 60).
- `MERGE_GATES_TIMEOUT_SECONDS` — wall-clock budget (default 3600).
- `MERGE_GATES_QUERY_FILE` — override GraphQL document path (default `agents/scripts/core/merge-gates.graphql`).
- `MERGE_GATES_CR_INSTALLED` — override the auto-detected CodeRabbit-installed flag (`true` / `false`). Auto-detection probes `repos/<owner>/<repo>/contents/.coderabbit.yaml` (and `.yml`); set explicitly when the config lives outside the repo or when running against a fork that has not yet enabled CR.
- `MERGE_GATES_CR_GRACE_POLLS` — polls to wait for CR to start (a review or `CodeRabbit` SUCCESS status) before falling through `NONE` to pass (default 10). Only consulted when `MERGE_GATES_CR_INSTALLED` is true / auto-detected as installed.
- `MERGE_GATES_STALE_REREVIEW_POLLS` — consecutive blocking-STALE polls on the same HEAD before the poller auto-posts `@coderabbitai review` once per HEAD (default 5). `0` disables the STALE auto-nudge. (No longer gates the CR=NONE early-nudge — that moved to the separate `MERGE_GATES_NONE_NUDGE_POLLS` knob below, `reduce-coderabbit-review-spend` Slice 2.)
- `MERGE_GATES_NONE_NUDGE_POLLS` — consecutive blocking-NONE polls on the same HEAD before the poller auto-posts `@coderabbitai review` once per HEAD (default **0 = disabled**). Decoupled from the STALE knob so the redundant first-poll NONE nudge can stay off — `auto_review` already reviews every push — while the STALE backstop stays on. Set > 0 to re-enable after an N-poll grace that lets auto_review post first. Carries across `MAX_POLLS=1` watcher cycles via `MERGE_GATES_PRIOR_NONE_STREAK` / `MERGE_GATES_PRIOR_NONE_HEAD` (mirror of the STALE carry, emitted on the `GATE_CARRY` line). Shares the once-per-HEAD guard with the STALE trigger, so at most one `@coderabbitai review` is posted per head SHA.
- `MERGE_GATES_TEST_ANSWER` — bats-only canned `ask_user_question` answer.

**Scope boundary**: the auto-`gh pr ready` + auto-merge path applies to the orchestrator, `git-janitor`, and `smatchet-merge-watcher`. No other caller has merge authority. The deleted spawned-child agents (`handoff-implementer`, `pr-iterator`) are gone per v1 of `docs/plans/shipped/github-tracker-backend.md`; the watcher runs as a host daemon, not a per-PR subprocess, so the spawned-child draft-only carve-out no longer applies.

Implementation: `agents/scripts/core/merge-gates.sh` (sourceable + CLI), `agents/scripts/core/merge-gates-prompt.sh` (`ask_user_question` shim), `agents/scripts/core/merge-gates.graphql`. Tests: `tests/bats/merge_gates.bats` + `tests/fixtures/merge_gates_*.json`.
