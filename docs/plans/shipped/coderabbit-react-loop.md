# PR-feedback react loop — CodeRabbit + CI failures (slim extension on `agentic-coding-handoff`)
<!-- plan-date: 2026-05-18 -->

> ## Status — HISTORICAL (2026-05-21 v2 cleanup)
>
> Describes the C++ CodeRabbit react loop that v1 PR1 of `docs/plans/shipped/github-tracker-backend.md` (PR #356, merge sha `b1d241bc`) **deleted in full**. The runtime referenced throughout this doc (`PrCommentWatcher`, `PrCheckRunWatcher`, `CoderabbitCommentClassifier`, `CiFailureClassifier`, the `dispatch_source` enum, sentinel-file protocol) no longer exists in the tree.
>
> Kept as historical reference because [`docs/plans/shipped/smatchet-merge-watcher.md`](archive/smatchet-merge-watcher.md) (the daemon-driven revival per the 2026-05-21 P1 backlog entry `644f822`) reuses several conceptual building blocks from this plan — the three-bucket CR-state model, the CR-finding classifier's Smatchet-invariant override table, the cascade-detection approach. Useful input for the watcher's design pass; do NOT treat the body below as a current implementation roadmap.
>
> **2026-05-18 partial-folding note (superseded by the 2026-05-21 ripout)**: The six `GitHubClient` PR / check-run / annotation / actions-log / rerun-workflow methods this plan originally scoped (phase 1 of the draft) shipped via [`agentic-coding-handoff.md`](agentic-coding-handoff.md) **H7** (PR #255, sha `37f4f2e`). H7's HTTP methods are also deleted now (per v1 PR1); a new `GitHubClient` exists in v1 PR2 (`cd66e28c`) but is tracker-only (issues + fields + labels), no PR / check-run / GraphQL surface.
>
> **Slug:** `coderabbit-react-loop` (file stays at this path; scope has widened beyond CodeRabbit since the doc was first committed — slug retained to avoid churn, file rename deferred unless future scope creep warrants it).
>
> **Canonical implementation runbook:** [`docs/plans/active/agentic-flow-implementation.md`](agentic-flow-implementation.md) sequences `agentic-triage-flow` phases T0–T9 + `agentic-coding-handoff` phases H0–H10 with copy-paste-ready build / test / commit / push / PR commands. **As of HEAD: T0–T9 + H0–H10 merged on `develop`** (last triage commit `321589c feat(agentic): T9 …`; last handoff commit `4b330ce feat(agentic): H10 …` PR #259). This plan's surviving phases sit **after H10**; do not start phase 1 until concrete demand materialises.
>
> **2026-05-18 re-validation pass.** § "Re-validation 2026-05-18 — H7 reality check + locked decisions" (immediately below) records architectural drift between the original plan prose and what H7 actually shipped, plus a user-locked decision batch from 2026-05-18 that supersedes the original `## User-locked decisions` table and parts of `## What is NOT yet covered` / `## Critical files` / `## Phased rollout` / `## Risks + mitigations` / `## Open questions`. **Read that section before doing anything else in this plan.**

## Re-validation 2026-05-18 — H7 reality check + locked decisions

Investigation against shipped H7 (PR #255 sha `37f4f2e`) + H10 (PR #259 sha `4b330ce`) found the in-flight architecture diverged from what the original plan prose assumed. Locked decisions from a 2026-05-18 user batch — these supersede the original prose where they conflict. Phase 1 starts from this section, not from `## Phased rollout` § Phase 1.

### Plumbing confirmed shipped (no rework needed)

- **All 6 H7 `GitHubClient` methods** ship per `Source_Core/include/GitHubClient.h:191-282` — `FetchPrComments`, `CreatePullRequest`, `FetchCheckRuns`, `FetchCheckRunAnnotations`, `FetchActionsJobLogs`, `RerunWorkflowRun`. Phase 0's dependency-shipped probe passes.
- **`ClaudeCodeLocalRunner` + sentinel-file IPC + worktree creation** ship per `Source_Core/include/ClaudeCodeLocalRunner.h` (H3 onward).
- **`PrCommentWatcher` core surface** ships per `Source_Core/include/PrCommentWatcher.h` (H7) — including the SQLite cursor persistence added at H10.
- **`pr-iterator` agent** ships at `agents/pr-iterator.md`. **`handoff-implementer` agent** ships at `agents/handoff-implementer.md`.
- **Macro gate is `SMATCHET_WITH_AGENTIC`** (the plan body says it correctly; calling it out explicitly because earlier sibling-plan drafts said `SMATCHET_WITH_AI`).
- **`agent_pr_watch` table** ships at H10 per `Source_Core/src/AgentProposalStore.cpp:142` — keyed on `proposal_id`, carries `pr_url`, `last_seen_comment_id_str TEXT`, `iteration_count`, `last_polled_at_sec`. Schema migration is at v2.

### Architecture corrections (supersede original prose)

1. **Watcher shape — Tick()-only, no thread per watcher.** `PrCommentWatcher` exposes `int Tick()` called once per scheduled-poll iteration; it owns no `std::thread` and no cancel atomic (per `PrCommentWatcher.h:18-28`). Both new components (`OpenPrRegistrar`, `PrCheckRunWatcher`) follow the same shape — each exposes `int Tick()`; the existing scheduled-poll worker invokes them per iteration. **Strike every reference to "worker thread; cancel atomic" in the original prose** (architecture diagram, critical-files table, phase-1 / phase-6 gates).

2. **Schema split — new `agent_open_pr_watch` sibling table.** `agent_pr_watch` is keyed on `proposal_id`; hand-pushed open-PR-scan rows have no proposal id. Use a new sibling table `agent_open_pr_watch` keyed on `pr_url`, carrying `last_seen_comment_id_str TEXT`, `last_seen_check_run_id INTEGER`, `iteration_count INTEGER`, `last_polled_at_sec INTEGER`. **Strike** the "add `origin TEXT` + `last_seen_check_run_id INTEGER` columns to `agent_pr_watch`" plan in original phase 1; **replace** with the new sibling-table creation. Both watchers iterate rows from both tables (`agent_pr_watch` for handoff-origin, `agent_open_pr_watch` for hand-pushed). Additive — no schema-version bump (H10 already shipped v2).

3. **First-delegate selection — always `handoff-implementer`.** Every dispatch path (CodeRabbit comment, CI build failure, CI ctest failure, CI coverage-gate) names `handoff-implementer` on `SEED.md`'s `## First delegate:` line. `handoff-implementer` reads `SEED.json` + (when present) the new `CHECK_RUN.json` sentinel, then routes internally based on a new `dispatch_source` discriminator field on `SEED.json` (enum: `proposal_implement | coderabbit_comment | ci_build_failure | ci_ctest_failure | ci_coverage_gate | ci_transient_rerun`). **This collapses 4 agent-prompt edits into 1.**
    - **Strike** the modifications to `agents/build-doctor.md`, `agents/debug-detective.md`, `agents/test-rig.md` in the original `## Critical files § Modified` table. Those agents are invoked through the existing AGENTS.md § Delegation table by `handoff-implementer` and need **no prompt changes** for this plan.
    - **Replace** with a single extension to `agents/handoff-implementer.md`: bump `version`, add `## Spawned-harness routing for non-proposal dispatch sources` section + dispatch-source enum + `CHECK_RUN.json` reader contract.
    - **Keep** the `agents/coderabbit-triage.md` v1→v2 bump with the `## Spawned-harness mode` section. `handoff-implementer` invokes it as the routed delegate for `coderabbit_comment` dispatches; the delegate must know to act as orchestrator when no parent orchestrator exists.

4. **Risk #1 reframing — H7 has no generic bot-skip filter.** Original Risk #1 said "`PrCommentWatcher`'s default bot-skip filter swallows CodeRabbit comments before classification". H7's actual filter is `AgenticHandoffController::IsHandoffBotComment` (per `PrCommentWatcher.h:22-27`), which **only** skips comments carrying the `<!-- smatchet-handoff -->` marker (Smatchet's own posts). CodeRabbit comments are NOT swallowed today — without this plan they would dispatch via the default path to `pr-iterator`, which is also not what we want. The classifier hook is an **addition ahead of the existing dispatch path**, not a "reorder before bot-skip". **Strike** Risk #1 in its current form; replace with: *"The classifier hook must run before the existing default dispatch to `pr-iterator`; the Smatchet-own-marker filter is unrelated."*

### User-locked decisions (2026-05-18 batch — supersedes the original table)

| Question | Decision (2026-05-18 batch) |
|---|---|
| Detection scope | **In-process only.** Both new watchers run inside Smatchet via the scheduled-poll worker. No daemon, no Windows Task Scheduler. Out-of-Smatchet detection is deferred to a future slice if demand surfaces. |
| Iteration-budget shape | **Three separate budgets.** `pr_iteration_budget` (handoff-origin), `coderabbit_react.iteration_budget_per_pr`, `ci_react.iteration_budget_per_pr` — different signals, different semantics, tunable independently. |
| First-delegate selection | **Always `handoff-implementer`.** Single entry point; routes internally based on `SEED.json.dispatch_source`. See correction #3 above. |
| Reply / rerun-workflow identity | **`gh auth status` user.** Short-circuit-reject replies + `gh workflow run` invocations appear authored by the human PAT owner. Matches the existing handoff PR-create flow. |
| Coverage advisory cutoff | **Hardcoded ignored list + manual config flip at 2026-05-30 cutoff.** Phase 5 hardcodes `"Coverage (windows-2022 + OpenCppCoverage)"` in `ci_react.ignored_check_names`; user flips it manually when `coverage.yml` becomes blocking. Document the date in `ConfigManager` comments. |
| Worktree GC (carry-forward) | Piggyback on the next-tick `gh api … /pulls/{n}` call — `state == "closed"` ⇒ gc the worktree. Confirm at phase 4. |
| Pillar-4 a11y (carry-forward) | Same keyboard-nav contract as existing toggles (Tab cycles, Space toggles, Enter on focused input). Audit at phase 8 against the pillar-4 in-scope list. |

The original `## User-locked decisions` table four rows above § Architecture still hold for: cloud-Claude execution (no), watcher shape (in-process — now reinforced above), auto-fix scope (full auto within whitelist), poll interval (30 min default for CodeRabbit watcher specifically). Nothing in that table is contradicted by the 2026-05-18 batch.

### Critical-files diff (supersedes original tables where they conflict)

- **Strike** the modifications to `agents/build-doctor.md`, `agents/debug-detective.md`, `agents/test-rig.md`. **Replace with** a single extension to `agents/handoff-implementer.md` (version bump + `## Spawned-harness routing` section + dispatch-source enum + `CHECK_RUN.json` reader).
- **Strike** the `PrCheckRunWatcher` description "Worker thread; cancel atomic". **Replace with**: "Tick()-only; called once per scheduled-poll iteration like `PrCommentWatcher`."
- **Strike** the `OpenPrRegistrar` description "Cancellable, owned by `AgenticHandoffController` as a `std::thread`". **Replace with**: "Tick()-only; `gh pr list` invoked once per `OpenPrRegistrar::Tick()` call from the scheduled-poll worker."
- **Strike** "add `origin TEXT` + `last_seen_check_run_id INTEGER` columns to `agent_pr_watch`" wherever the original prose mentions it (`## What is NOT yet covered` item #1, `## Reuse` table, `## Phased rollout` phase 1). **Replace with**: create new sibling table `agent_open_pr_watch (pr_url TEXT PRIMARY KEY, last_seen_comment_id_str TEXT NOT NULL DEFAULT '', last_seen_check_run_id INTEGER NOT NULL DEFAULT 0, iteration_count INTEGER NOT NULL DEFAULT 0, last_polled_at_sec INTEGER NOT NULL DEFAULT 0)`. Additive table (no schema-version bump).
- **Add** to `the deleted handoff-envelope section` § Sentinel files: `CHECK_RUN.json` (writer: runner / classifier; reader: handoff-implementer; one-line role: CI-failure cause payload — check-run name, conclusion, top N annotations, last N log lines — written into the worktree before spawn for `ci_*` dispatch sources). And add `dispatch_source` field to the `SEED.json` shape in the same section.

### Revised phase-1 sequence (supersedes original `## Phased rollout` § Phase 1)

1. Plan-lock claim (`bash scripts/dev/lock-claim.sh coderabbit-react-loop .claude/coderabbit-react-loop.write-set.txt`) — done in the wip(plan) commit that lands this revision.
2. Add `agent_open_pr_watch` table to `AgentProposalStore` schema (additive; no version bump).
3. Implement `OpenPrRegistrar` as a Tick()-only class — `OpenPrRegistrar::Tick()` runs `gh pr list --base <branch> --state open --json number,headRefName,headRefOid` via `SubprocessCapture`, upserts each returned row into `agent_open_pr_watch`.
4. Wire `OpenPrRegistrar::Tick()` into the existing scheduled-poll worker (the same worker that calls `PrCommentWatcher::Tick`) so all three components share one thread.
5. doctest: `OpenPrRegistrar.test.cpp` — upsert idempotency, row dedupe on re-run, fixture-driven `gh pr list` output via `SubprocessCapture` injection (no real subprocess).

### Open questions — closed by the 2026-05-18 batch

The original `## Open questions` items #1, #4, #5, #6, #7 are closed by the decisions above. Item #2 (first-delegate selection) is closed by correction #3. Item #3 (auto-GC strategy) carries forward into phase 4 with the recommended "piggyback on next-tick `gh api … /pulls/{n}`" approach. No remaining unresolved questions before phase 1.

## Context

Earlier this session two new artefacts shipped:
- `.coderabbit.yaml` — baseline CodeRabbit bot config (chill profile, vendored-path filter, `path_instructions` citing C++14-hard / dual-target / `LOG_*` / `TrackerHttpClient` / pillar 2).
- `agents/coderabbit-triage.md` — read-only investigator agent that ingests PR-bot feedback via `gh api`, runs an 18-rule override table to reject invariant-violating suggestions, classifies survivors, and emits handoff packets.

User wants a **local** auto-react loop equivalent to "fix the things CodeRabbit flagged" without a cloud Claude CLI. Initial draft proposed an out-of-Smatchet bash daemon + Windows Task Scheduler + SessionStart-hook surface.

**Rereading the existing design docs invalidates most of that draft.** The two in-flight plans collectively cover the polling, the worktree spawn, the sentinel-file IPC, the iteration loop, the agent file layout — every primitive the daemon shape would have re-implemented. This plan therefore **shrinks to a thin extension** that wires the already-planned `PrCommentWatcher` to CodeRabbit feedback specifically, **and adds a sibling `PrCheckRunWatcher` + `CiFailureClassifier` so the same machinery also auto-fixes red CI** (build-doctor for cmake/link, debug-detective for ctest failures, test-rig for coverage-gate). The marginal cost of folding CI in is ~one phase — the polling + sentinel + spawn plumbing is already justified by CodeRabbit, so the second classifier is essentially free.

### What already exists in the in-flight plans

| Need | Already covered by |
|---|---|
| In-process PR-comment polling thread | `PrCommentWatcher` (`agentic-coding-handoff` phase H7 in the unified runbook) — polls `GET /repos/{o}/{r}/pulls/{n}/comments` + `GET /repos/{o}/{r}/issues/{n}/comments` every `pr_poll_interval_sec` (default 120 s), SQLite cursor `agent_pr_watch.last_seen_comment_id` |
| Subprocess spawn for local `claude` | `ClaudeCodeLocalRunner` (`agentic-coding-handoff` phase 3) using `SubprocessCapture` lifted from `P4Blame` (phase 1) |
| Worktree creation per task | `ClaudeCodeLocalRunner::CreateWorktree` (`git worktree add .claude/worktrees/agent-<id> -b agent/<id>/<slug> origin/develop`) |
| Sentinel-file IPC (`SEED.json`, `CLARIFICATION_NEEDED.json`, `USER_RESPONSE.json`, `RUN_RESULT.json`, `PR_URL.txt`) | `the deleted handoff-envelope section` (phase 2) |
| Iteration loop (re-spawn harness when new comments arrive) | `PrCommentWatcher` → `runner.Resume(proposalId, formattedComments + diff)` |
| GitHub HTTP wrapper | `GitHubClient` (`AddIssueCommentPlain` from `agentic-triage-flow` T2; `FetchPrComments` + `CreatePullRequest` from `agentic-coding-handoff` H7) |
| Audit trail per state transition | `BackendAuditTrail::AppendEvent` already wired by `AgenticHandoffController` (`agentic-coding-handoff` phase 4) |
| Live-progress channel for the spawned harness | `.progress.log` tail surfaced in `SmatchetAgentHandoffUi` panel (`agentic-coding-handoff` phase 8) |
| PR-iteration agent | `agents/pr-iterator.md` (`agentic-coding-handoff` phase 7) — classifies each unresolved comment (build / test / style / logic / design) and routes |
| CodeRabbit-specific triage + 18-rule override table | `agents/coderabbit-triage.md` (already shipped this session) |
| CodeRabbit bot config | `.coderabbit.yaml` (already shipped this session) |
| In-process budget enforcement | `pr_iteration_budget` in `ConfigManager`, enforced inside `PrCommentWatcher` |
| Iterating-state machine | `HarnessRunState` enum already lists `Iterating` (`agentic-coding-handoff` phase 4) |

### What is **NOT** yet covered (this plan adds)

1. **Open-PR registration scan.** `PrCommentWatcher` only watches PRs in `agent_pr_watch` — populated when `AgenticHandoffController` opens a PR via a handoff. **PRs the user hand-pushed are never watched.** CodeRabbit-react must register every open PR (whether handoff-origin or hand-pushed) so the watcher polls all of them. Lifted to a shared `OpenPrRegistrar` periodic task because both the comment watcher and the new check-run watcher need the same PR list.
2. **Comment-source classifier hook.** `PrCommentWatcher`'s default behaviour is "skip bot comments, dispatch non-bot comments to `pr-iterator`". CodeRabbit feedback is bot-authored — needs an inclusive filter for `user.login == "coderabbitai[bot]"` plus dispatch to `coderabbit-triage` (not `pr-iterator`).
3. **Ad-hoc worktree for non-handoff PRs.** Hand-pushed PRs have no `ClaudeCodeLocalRunner`-owned worktree. Need on-demand worktree creation (`git worktree add .claude/worktrees/coderabbit-pr<N> -b coderabbit/pr<N>/iter<n> origin/<headRefName>`) plus cleanup on PR close/merge. Same ad-hoc worktree is reused by the CI-failure dispatch path (one worktree per PR per iteration, regardless of trigger source).
4. **`coderabbit-triage` v2 — spawned-harness mode.** Today the agent emits handoff packets to a parent orchestrator. When invoked **inside** a spawned `claude` subprocess (no parent orchestrator), it must act as orchestrator itself: run override-rules, dispatch fixes directly to specialists, run the slice-boundary build + test, write `RUN_RESULT.json`, push back to the PR.
5. **Short-circuit reject for invariant violations.** Most CodeRabbit suggestions that violate Smatchet rules (use `std::optional`, etc.) can be rejected **without** spawning a harness — pure C++ classifier + 1-line PR reply via `GitHubClient::AddIssueCommentPlain`. Saves the cost of every full spawn for trivially-rejectable suggestions.
6. **`coderabbit-react.*` + `ci-react.*` command surfaces + config blocks** alongside the existing `handoff.*` commands.
7. **CI-failure auto-fix path.** Sibling to the CodeRabbit path. New `PrCheckRunWatcher` polls `GET /repos/{o}/{r}/commits/{head_sha}/check-runs` per tracked PR each tick; on `conclusion == "failure"`, `CiFailureClassifier` reads the check-run name + first N annotations + last N log lines, classifies (build / link / ctest / coverage-gate / unknown), routes:
    - `build-and-test` failing with cmake/link/MSYS2/lld error → spawn `build-doctor` as first delegate
    - `build-and-test` failing with ctest assertion / sanitizer hit → spawn `debug-detective` as first delegate (pause-loop applies — debug-detective halts at round boundary for user review per AGENTS.md § Debug-mode pause-loop)
    - `coverage-gate` failing (missing paired test delta) → spawn `test-rig` as first delegate
    - `coverage` advisory failing → log + skip (advisory until 2026-05-30 per `coverage.yml`)
    - Unknown check-run name → log + skip + emit `Self-improvement` entry so the table can grow
    The classifier short-circuits without spawning when the failure looks transient (network timeout in cpr-fetched dep, MSYS2 mirror hiccup) — it posts a PR comment "transient failure detected; re-running CI" and re-runs the check via `gh workflow run` (no spawn cost). Transient detection is a fingerprint-match (set of known-flaky stderr substrings); false-positives default to "treat as real" so the user is never left with a silently-ignored failure.

### User-locked decisions (carrying forward)

| Question | Decision (this session) |
|---|---|
| Cloud Claude execution | **No.** Local-only via `ClaudeCodeLocalRunner` (already the `agentic-coding-handoff` default). |
| Watcher shape | **In-process `PrCommentWatcher`** (revised from earlier "out-of-Smatchet daemon" pick — see below). The user's prior "hybrid: daemon detects, session fixes" was answered before they reminded me of the in-flight plans; the in-process `PrCommentWatcher` is functionally equivalent **as long as Smatchet.exe runs while you want detection to happen**. Cross-checked in § Open questions. |
| Auto-fix scope | **Full auto within whitelist.** Subsystem specialists fix unattended within the per-finding allowed write set; architect / cross-cutting items halt for user. |
| Poll interval | **30 min** (override `pr_poll_interval_sec` default of 120 s for the CodeRabbit-react watcher specifically — CodeRabbit fires on push, not chat-fast, so 30 min is well-matched to its cadence and keeps the gh rate-limit budget healthy). |

## Architecture

```
agentic-coding-handoff (already planned)            this plan adds
┌─────────────────────────────────────┐            ┌──────────────────────────────────┐
│ AgenticHandoffController            │            │ OpenPrRegistrar (shared)         │
│  ├─ PrCommentWatcher (poll loop)    │◄───────────┤  gh pr list → agent_pr_watch     │
│  │    ├─ poll comments per PR       │            │  upsert (every interval; both    │
│  │    ├─ classify via               │            │  watchers iterate the rows)      │
│  │    │   PrCommentClassifier       │◄───────────┤                                  │
│  │    └─ runner.Resume(...)         │            │ PrCommentClassifier interface +  │
│  │                                  │            │  CoderabbitCommentClassifier     │
│  └─ PrCheckRunWatcher (new sibling) │◄───────────┤                                  │
│       ├─ poll check-runs per PR     │            │ PrCheckRunClassifier interface + │
│       ├─ classify via               │            │  CiFailureClassifier (cmake-err →│
│       │   PrCheckRunClassifier      │◄───────────┤   build-doctor; ctest-fail →     │
│       └─ runner.Spawn(...)          │            │   debug-detective; coverage-gate │
└─────────────────────────────────────┘            │   → test-rig; transient → rerun) │
                  │                                └──────────────────────────────────┘
                  │
        ┌─────────┴────────────┐
        │ comment dispatch     │ check-run dispatch
        ▼                      ▼
┌─────────────────────────────────────┐            ┌──────────────────────────────────┐
│ ClaudeCodeLocalRunner (already)     │◄───────────┤ ad-hoc worktree creation for     │
│  └─ spawn claude in PR worktree     │            │  hand-pushed PRs (no handoff     │
│       └─ first delegate =           │◄───────────┤  origin worktree exists);        │
│         agents/coderabbit-triage    │            │  shared between CodeRabbit and   │
│         (v2 — spawned-harness mode) │            │  CI-failure dispatch (one        │
│         OR build-doctor             │            │  worktree per PR per iteration)  │
│         OR debug-detective          │            │                                  │
│         OR test-rig                 │            │ ConfigManager.coderabbit_react + │
│         (per classifier dispatch)   │            │  ConfigManager.ci_react blocks + │
└─────────────────────────────────────┘            │  Preferences toggles             │
                  │                                └──────────────────────────────────┘
                  │ short-circuit (override-rule rejection
                  │  OR transient-failure re-run)
                  ▼
              GitHub PR reply / gh workflow run (no spawn cost)
```

**No new daemon, no new SessionStart hook, no new bash watcher script** — `PrCommentWatcher` + `PrCheckRunWatcher` cover in-process polling on `std::thread`s. The earlier draft's `scripts/dev/coderabbit-watch.sh` + `start-coderabbit-watch.ps1` + Windows Task Scheduler approach is **abandoned** as duplicate functionality.

## Critical files

### New
| Path | Purpose |
|---|---|
| `Source_Core/{include,src}/OpenPrRegistrar.{h,cpp}` | Shared periodic task — every interval, `gh pr list --base <branch> --state open --json number,headRefName,headRefOid` (subprocess via `SubprocessCapture`), upsert each row into `agent_pr_watch` with `origin = "open-pr-scan"`. Consumed by both `PrCommentWatcher` (OpenPrScan mode) and `PrCheckRunWatcher`. Cancellable, owned by `AgenticHandoffController` as a `std::thread` |
| `Source_Core/include/PrCommentClassifier.h` | Abstract interface — `virtual ClassificationResult Classify(const PrComment&)` returns `{dispatch | reject_short_circuit | skip}` + target-agent name + optional pre-built short-circuit reply |
| `Source_Core/{include,src}/CoderabbitCommentClassifier.{h,cpp}` | Concrete classifier — recognises `user.login == "coderabbitai[bot]"`, applies the 18-rule override table in C++ for short-circuit reject (saves a spawn for "use `std::optional`" suggestions), dispatches survivors to `coderabbit-triage` |
| `Source_Core/{include,src}/CoderabbitCommentClassifierPure.{h,cpp}` | Pure helpers — body parser for CodeRabbit's `🛠️ ⚠️ 💡 🧹` icons, `_Actionable comments posted: N_` header parser, `suggestion` block extractor. Pure-helper TU split per AGENTS.md § "Pure-helper TU-split recipe" so doctest rig can exercise without GitHub HTTP |
| `Source_Core/include/PrCheckRunClassifier.h` | Abstract interface — `virtual ClassificationResult Classify(const PrCheckRun&)` returns `{dispatch | rerun_transient | skip}` + target-agent name + (for `rerun_transient`) the workflow ID to re-fire |
| `Source_Core/{include,src}/PrCheckRunWatcher.{h,cpp}` | New sibling to `PrCommentWatcher`. Per tick, iterates `agent_pr_watch` rows; for each, `GET /repos/{o}/{r}/commits/{head_sha}/check-runs` via `GitHubClient::FetchCheckRuns` (new method). Diffs against `agent_pr_watch.last_seen_check_run_id` cursor; dispatches new `conclusion == "failure"` rows through the registered `PrCheckRunClassifier`. Worker thread; cancel atomic |
| `Source_Core/{include,src}/CiFailureClassifier.{h,cpp}` | Concrete classifier for the standard Smatchet CI matrix. Reads check-run name + first N annotations + last N log lines; classifies into `{build-doctor, debug-detective, test-rig, transient-rerun, skip}`. Annotation/log fetch via `GitHubClient::FetchCheckRunAnnotations` (new) + `GitHubClient::FetchActionsJobLogs` (new) |
| `Source_Core/{include,src}/CiFailureClassifierPure.{h,cpp}` | Pure helpers — check-run-name → broad-category mapping table (`build-and-test` → cmake-or-ctest, `coverage-gate` → coverage, `coverage` → advisory), cmake/link/ctest/sanitizer fingerprint matchers, transient-flake fingerprint list (cpr network-timeout, MSYS2 mirror DNS, FetchContent ZIP-retry). Pure-helper TU split — same recipe |
| `Source_Core/src/Commands/Builtin/BuiltinCommands_Coderabbit.cpp` | `coderabbit-react.start`, `.stop`, `.status`, `coderabbit-react.poll-now <pr>` (manual fire) |
| `Source_Core/src/Commands/Builtin/BuiltinCommands_CiReact.cpp` | `ci-react.start`, `.stop`, `.status`, `ci-react.poll-now <pr>`, `ci-react.rerun <pr>` (manual transient re-fire) |
| `tests/Source_Core/CoderabbitCommentClassifierPure.test.cpp` | Override-rule table coverage (each of the 18 rules has at least one positive + one negative case), icon parser, suggestion-block extractor |
| `tests/Source_Core/CoderabbitCommentClassifier.test.cpp` | Wires pure helpers to the override table; dispatch survivors covering ≥3 distinct subsystem targets |
| `tests/Source_Core/PrCommentWatcher_OpenPrScan.test.cpp` | New TU for open-PR-scan mode (separate from the existing `PrCommentWatcher.test.cpp` so the two modes have independent failure footprints) |
| `tests/Source_Core/OpenPrRegistrar.test.cpp` | Upsert idempotency against fixture `gh pr list` output; cancellation; row dedupe on re-run |
| `tests/Source_Core/PrCheckRunWatcher.test.cpp` | Cursor advance, classifier injection, failure-only filter, fixture-driven dispatch (no real HTTP) |
| `tests/Source_Core/CiFailureClassifierPure.test.cpp` | Check-run-name table coverage, cmake-error fingerprint, ctest-fail fingerprint, sanitizer-hit fingerprint, transient-flake fingerprint, unknown-name fallback |
| `tests/Source_Core/CiFailureClassifier.test.cpp` | Wires pure helpers to annotation/log fetch; routes the 5 buckets correctly against fixture check-run payloads |
| `tests/fixtures/coderabbit_comments_sample.json` | Recorded `gh api … /pulls/{n}/comments` payload featuring at least one of each: actionable suggestion that should dispatch, nitpick that should dispatch via mechanic, suggestion that should short-circuit reject per override rule #1 |
| `tests/fixtures/check_runs_failed_sample.json` | Recorded `gh api … /commits/{sha}/check-runs` payload with one `build-and-test` cmake failure, one `build-and-test` ctest failure, one `coverage-gate` failure, one `coverage` advisory failure, one transient cpr-timeout failure, one unknown check-run name |
| `tests/fixtures/check_run_annotations_sample.json` | Annotations + log slices for each of the above check-runs, used by `CiFailureClassifier.test.cpp` |
| `tests/fixtures/stub-coderabbit-claude.sh` | Bash script masquerading as `claude` for the spawned-harness-mode test — emits canned stream-json + writes `RUN_RESULT.json` with one fix applied |
| `tests/fixtures/stub-ci-claude.sh` | Bash script masquerading as `claude` for the CI-failure spawned-harness test — emits canned stream-json that simulates `build-doctor` cmake re-edit + `cmake --build` exit-0 + `RUN_RESULT.json` |
| `scripts/dev/test-coderabbit-react.sh` | Bucket-A CLI smoke — starts the react loop, injects fixture comments, asserts the watcher dispatches + the stub claude makes a fix + the loop pushes |
| `scripts/dev/test-ci-react.sh` | Bucket-A CLI smoke — starts the CI react loop, injects fixture failed check-runs, asserts the watcher dispatches to the right specialist + stub claude lands the fix + the loop pushes; separately asserts transient-flake re-runs `gh workflow run` instead of spawning |

### Modified (existing)
| Path | Change |
|---|---|
| `Source_Core/include/PrCommentWatcher.h` | Add `enum class WatchMode { OriginTracking, OpenPrScan }`; add classifier-registration API `void RegisterClassifier(std::unique_ptr<PrCommentClassifier>)`; default behaviour preserved when no classifier registered. **No longer owns OpenPrScan registration** — that lifted to `OpenPrRegistrar` |
| `Source_Core/src/PrCommentWatcher.cpp` | In `OpenPrScan` mode, iterate `agent_pr_watch` rows where `origin = "open-pr-scan"` (rows populated by `OpenPrRegistrar`, not by this watcher anymore); inject classifier in the dispatch path |
| `Source_Core/src/AgenticHandoffController.cpp` | Own the `OpenPrRegistrar` thread + the new `PrCheckRunWatcher`. Register `CoderabbitCommentClassifier` when `coderabbit_react.enabled = true`; register `CiFailureClassifier` when `ci_react.enabled = true`; gate the whole block by `#if SMATCHET_WITH_AGENTIC` |
| `Source_Core/src/ClaudeCodeLocalRunner.cpp` | Add ad-hoc worktree path for PRs without handoff-origin worktree (resolved by lookup in `agent_pr_watch.origin` — if `"open-pr-scan"`, create `coderabbit-pr<N>/iter<n>` worktree off `origin/<headRefName>`). Same path is reused for CI-failure spawns (the worktree namespace stays `coderabbit-pr*` even when the trigger was CI — single worktree per PR per iteration regardless of which classifier dispatched, so the spawned harness can see all in-flight signal sources for the PR in one place) |
| `Source_Core/include/ConfigManager.h` + `.cpp` | New blocks `coderabbit_react` + `ci_react` (no schema-version bump — held until phase 8 verification per AGENTS.md § "Schema-version bumps") |
| `Source_Core/src/SmatchetPreferencesUi.cpp` | New "CodeRabbit react loop" toggle + interval; new "CI react loop" toggle + interval + transient-rerun toggle |
| `agents/coderabbit-triage.md` | Bump `version: 1` → `2`. Add `## Spawned-harness mode` section: when invoked inside a spawned `claude` subprocess with no parent orchestrator (detected by `SEED.json` absence + `pr-iteration-mode` env var presence), act as orchestrator — execute the routed fix as `Edit` calls, run slice-boundary `cmake --build` + `scripts/dev/test-all.sh`, write `RUN_RESULT.json`, exit with success. Outside spawned-harness mode (existing behaviour), emit handoff packets to parent orchestrator unchanged |
| `agents/build-doctor.md` | Bump `version` per AGENTS.md § Agent versioning. Add a short `## Spawned-harness mode` paragraph parallel to `coderabbit-triage`: when invoked inside a spawned harness with `ci-failure-mode` env var, treat the seed's `CHECK_RUN.json` payload as the failure-cause source, fix in-place, run `cmake --build` to confirm, write `RUN_RESULT.json`, push commit |
| `agents/debug-detective.md` | Same shape — add `## CI-failure spawned-harness mode` paragraph. **Note:** the debug-detective pause-loop (AGENTS.md § Debug-mode pause-loop) still applies — when dispatched for a ctest/sanitizer failure inside a spawned harness, the agent halts at round boundary and writes `CLARIFICATION_NEEDED.json` rather than auto-fixing. CI-failure auto-fix for behavioural regressions is **always** user-gated; only build-system failures (build-doctor scope) auto-land |
| `agents/test-rig.md` | Same shape — add `## CI-failure spawned-harness mode` for `coverage-gate` failures (missing paired test delta) |
| `AGENTS.md` | Addendum to § Handoff envelope: `pr-iterator` is the **default** first-delegate inside a spawned harness; `coderabbit-triage` / `build-doctor` / `debug-detective` / `test-rig` are alternates when the appropriate classifier (`PrCommentClassifier` / `PrCheckRunClassifier`) routes the trigger to them. Add the `CHECK_RUN.json` sentinel-file definition to the existing sentinel-vocabulary subsection. One-paragraph + one table-row addition, no new section |
| `.gitignore` | Add `.claude/worktrees/coderabbit-pr*` (the ad-hoc worktree namespace — used for both CodeRabbit and CI-failure spawns) |

### `ConfigManager` `coderabbit_react` + `ci_react` blocks
```json
{
  "coderabbit_react": {
    "enabled": false,
    "poll_interval_sec": 1800,
    "watched_base_branches": ["develop"],
    "bot_logins": ["coderabbitai[bot]"],
    "short_circuit_reject_enabled": true,
    "auto_dispatch_fixes": true,
    "iteration_budget_per_pr": 5,
    "ad_hoc_worktree_root": ".claude/worktrees"
  },
  "ci_react": {
    "enabled": false,
    "poll_interval_sec": 600,
    "watched_base_branches": ["develop"],
    "watched_check_names": [
      "Windows + MSYS2 UCRT64",
      "Windows + MSYS2 UCRT64 (SMATCHET_WITH_AGENTIC=OFF)",
      "Windows + MSYS2 UCRT64 (SMATCHET_WITH_WHISPER=OFF)",
      "Test-delta gate"
    ],
    "ignored_check_names": ["Coverage (windows-2022 + OpenCppCoverage)"],
    "auto_dispatch_build_doctor": true,
    "auto_dispatch_test_rig": true,
    "auto_dispatch_debug_detective": false,
    "transient_rerun_enabled": true,
    "transient_rerun_max_per_pr": 2,
    "iteration_budget_per_pr": 5,
    "annotation_fetch_count": 20,
    "log_tail_lines": 200
  }
}
```

`ci_react.poll_interval_sec` defaults to 10 min (vs CodeRabbit's 30 min) because CI completes in 5–15 min on Smatchet's build matrix and slower polling means longer dead time after red. `auto_dispatch_debug_detective` defaults **off** — behavioural / ctest regressions auto-spawn the agent but the agent itself halts at its first pause-loop boundary, so practically the user reviews every behavioural fix. The flag toggles whether to spawn at all.

`OpenPrRegistrar.poll_interval_sec` shares one config key — both watchers iterate rows the registrar produced, so its cadence governs how quickly a newly-opened PR enters the react surface. Defaults to `min(coderabbit_react.poll_interval_sec, ci_react.poll_interval_sec)` (= 600 s when both enabled).

## Reuse (do not re-implement)

| Need | Use |
|---|---|
| In-process poll loop | `PrCommentWatcher::Tick` (`agentic-coding-handoff` phase 7) — pattern copied for new `PrCheckRunWatcher::Tick` |
| Cursor persistence | `agent_pr_watch.last_seen_comment_id` (already in `agentic-coding-handoff` schema) — add `origin TEXT` + `last_seen_check_run_id INTEGER` columns in this plan (both additive — no version bump) |
| Subprocess spawn | `SubprocessCapture::Run` (`agentic-coding-handoff` phase 1) |
| Worktree creation | `ClaudeCodeLocalRunner::CreateWorktree` (`agentic-coding-handoff` phase 3) — extend to take a `WorktreeOrigin` enum; shared between CodeRabbit + CI-failure spawns |
| Sentinel-file IPC | `the deleted handoff-envelope section` (`agentic-coding-handoff` phase 2) — extend the sentinel vocabulary with `CHECK_RUN.json` (failure-cause payload from `CiFailureClassifier`, written into the worktree before the spawn so the harness reads it as its primary fact source) |
| GitHub HTTP — PR comments | `GitHubClient::FetchPrComments` (added by `agentic-coding-handoff` H7 per its § Reused-utilities table — `agentic-triage-flow` T2 owns the write methods but NOT this read method, despite the line getting brushed against both plans during scoping) |
| GitHub HTTP — reply to comment | `GitHubClient::AddIssueCommentPlain` (`agentic-triage-flow` T2 — confirmed at `docs/plans/active/agentic-triage-flow.md:64`) — also used by short-circuit-reject path |
| GitHub HTTP — check-runs | **New methods on `GitHubClient`, folded into `agentic-coding-handoff` H7 scope** — `FetchCheckRuns(headSha)`, `FetchCheckRunAnnotations(checkRunId)`, `FetchActionsJobLogs(jobId, tailLines)`, `RerunWorkflowRun(runId)`. All follow the `TrackerHttpRequestWithRetry` + `BackendAuditTrail` pattern that `GitHubClient`'s existing methods use. Land alongside `FetchPrComments` + `CreatePullRequest` in H7 so the `GitHubClient` surface stays cohesive and arrives in a single PR. This plan documents the surface; the implementation is owned by the H7 implementer. Phase 0 here gates on the merged H7 PR exposing all 6 methods (probe: `grep -nE 'FetchCheckRuns\|RerunWorkflowRun' Source_Core/include/GitHubClient.h`). |
| Audit trail | `BackendAuditTrail::AppendEvent` (existing) |
| Worker thread + cancel atomic | `AppController::LaunchBackgroundTask` (existing) |
| Worker → UI hand-off | `MainThreadDispatcher::PostToMainThread` (existing) |
| Toast | `SmatchetToastManager::Push` (existing) |
| Override-rule table | `agents/coderabbit-triage.md` § "Override rules" (18 entries, shipped — referenced by `CoderabbitCommentClassifier::ShouldShortCircuitReject` in C++) |
| Routing table | `agents/coderabbit-triage.md` § "Routing table" (referenced for the spawned-harness dispatch) |
| CI check-run name → category mapping | `.github/workflows/build-and-test.yml`, `coverage-gate.yml`, `coverage.yml` (read at phase 5 to build the `CiFailureClassifierPure` name table — do not hardcode without cross-checking the workflow files) |
| `gh pr list --json` query shape | `agents/git-janitor.md` pre-flight (existing pattern) |
| `gh workflow run` invocation | `gh` CLI standard — used by transient-flake re-run path |
| Plan-lock pre-flight | `bash scripts/dev/locks-show.sh` (existing) |

## Out of scope (deferred or covered elsewhere)

- **Out-of-Smatchet daemon / Task Scheduler.** Abandoned — `PrCommentWatcher` in-process polling makes it redundant for any flow where Smatchet.exe is running. If the user wants detection while Smatchet is closed, a future slice can add an out-of-process `scripts/dev/coderabbit-watch.sh` that only writes to `agent_pr_watch` and lets Smatchet's in-process watcher pick up the row on next start. **Not in this plan.**
- **SessionStart hook in `.claude/hooks/`.** Abandoned for the same reason — `PrCommentWatcher` already surfaces new comments via `SmatchetAgentHandoffUi` while Smatchet runs.
- ~~**PR thread-resolve via GraphQL.** Same scope as `agentic-coding-handoff`'s out-of-scope — separate slice.~~ **Landed in phase 7** (`GitHubClient::ResolveReviewThread` + `LookupReviewThreadIdForComment`). See § Cross-cutting: merge-gates interaction.
- **Cloud Claude execution.** User explicitly rejected; `ClaudeCodeLocalRunner` is the only runner this plan uses (`agentic-coding-handoff`'s `ICodingHarnessRunner` interface allows future cloud runners but they're not wired here).
- **Cross-repo PRs.** Same scope as `agentic-coding-handoff` — only PRs in the current Smatchet repo are watched.
- **Webhook-driven CI react.** Push-mode (GitHub webhook `check_run.completed`) would react instantly but needs a public HTTPS endpoint and signature verification. Polling at 10-min cadence is good enough — flagged as future slice if instant-react matters.
- **Auto-fix for `debug-detective` ctest dispatches.** `auto_dispatch_debug_detective` defaults off. The agent inherits the AGENTS.md § Debug-mode pause-loop, so even when on, it halts at first round boundary — practically every behavioural-regression fix is human-gated. The flag toggles whether to spawn at all.
- **`.coderabbit.yaml` schema changes.** Already shipped; only revisit if a phase uncovers a missing `path_instructions` entry.

## Phased rollout

Hard dependencies: `agentic-triage-flow` phases 0–2 (`GitHubClient` read+write — **including the 4 new check-run methods promoted to the companion plan**) and `agentic-coding-handoff` phases 0–7 (`PrCommentWatcher` + `ClaudeCodeLocalRunner` + sentinel-file protocol + `pr-iterator`) must have shipped. Plan-lock claim slug: `coderabbit-react-loop`. Run `bash scripts/dev/locks-show.sh` before phase 1; `AgenticHandoffController.cpp` + `PrCommentWatcher.cpp` + `AGENTS.md` + `agents/build-doctor.md` + `agents/debug-detective.md` + `agents/test-rig.md` are head files — coordinate with whoever holds those if active.

| Phase | Scope | Key files | Gate |
|---|---|---|---|
| 0 | Plan-lock claim, doc move to canonical (`docs/plans/shipped/coderabbit-react-loop.md` — already done), ADRs for `OpenPrRegistrar` + `PrCheckRunWatcher` design (`docs/adr/00NN-prcheckrunwatcher.md`, `docs/adr/00NN-open-pr-registrar.md`), glossary additions, dependency-shipped check (`gh pr view` for the `agentic-coding-handoff` phase-7 PR + the `agentic-triage-flow` phase-2 PR carrying the new `GitHubClient` check-run methods, both must be merged) | `docs/plans/shipped/coderabbit-react-loop.md`, `docs/adr/00NN-…md`, `docs/CONTEXT.md` | doc-only |
| 1 | `OpenPrRegistrar` extracted as standalone periodic task (lifted out of the original `PrCommentWatcher::OpenPrScan` mode body — the shared registrar lets `PrCheckRunWatcher` consume the same `agent_pr_watch` rows without each watcher running its own `gh pr list`). Add `origin TEXT` + `last_seen_check_run_id INTEGER` columns to `agent_pr_watch` (both additive). Doctest covers upsert idempotency + cancellation | `Source_Core/{include,src}/OpenPrRegistrar.{h,cpp}`, `Source_Core/src/AgenticHandoffController.cpp` (own the registrar thread), `tests/Source_Core/OpenPrRegistrar.test.cpp` | dual-target build + ctest |
| 2 | `PrCommentClassifier` interface + `CoderabbitCommentClassifierPure` (icon / suggestion-block / actionable-header parsers — pure helpers per AGENTS.md "Pure-helper TU-split recipe"). Doctest covers every CodeRabbit comment shape from the recorded fixture | `Source_Core/include/PrCommentClassifier.h`, `Source_Core/{include,src}/CoderabbitCommentClassifierPure.{h,cpp}`, `tests/Source_Core/CoderabbitCommentClassifierPure.test.cpp`, `tests/fixtures/coderabbit_comments_sample.json` | build + ctest |
| 3 | `CoderabbitCommentClassifier` concrete impl — wires the pure helpers to the 18-rule override table; for each rule that fires, builds a short-circuit-reject reply body that cites the rule number. Doctest covers each of the 18 rules + at least one survivor that dispatches to `coderabbit-triage` | `Source_Core/{include,src}/CoderabbitCommentClassifier.{h,cpp}`, `tests/Source_Core/CoderabbitCommentClassifier.test.cpp` (extends fixture) | build + ctest |
| 4 | Extend `PrCommentWatcher` with `WatchMode` enum + classifier-registration API. Default `OriginTracking` behaviour unchanged. In `OpenPrScan` mode, iterate rows the new `OpenPrRegistrar` (phase 1) wrote — this watcher no longer runs `gh pr list` itself | `Source_Core/include/PrCommentWatcher.h`, `Source_Core/src/PrCommentWatcher.cpp`, `tests/Source_Core/PrCommentWatcher_OpenPrScan.test.cpp` | build + ctest |
| 5 | `PrCheckRunClassifier` interface + `CiFailureClassifierPure` (check-run-name table, cmake/link/ctest/sanitizer/transient fingerprints — pure helpers). Read `.github/workflows/build-and-test.yml` + `coverage-gate.yml` + `coverage.yml` at phase start to build the name table; do not hardcode against memory | `Source_Core/include/PrCheckRunClassifier.h`, `Source_Core/{include,src}/CiFailureClassifierPure.{h,cpp}`, `tests/Source_Core/CiFailureClassifierPure.test.cpp`, `tests/fixtures/check_runs_failed_sample.json`, `tests/fixtures/check_run_annotations_sample.json` | build + ctest |
| 6 | `CiFailureClassifier` concrete + `PrCheckRunWatcher` (poll-loop, cursor advance via `last_seen_check_run_id`, dispatch hook). Annotation/log fetch via the new `GitHubClient` methods (promoted to `agentic-triage-flow` phase 2). Wire dispatch into `AgenticHandoffController` — `CiFailureClassifier` registered when `ci_react.enabled = true`. Per-target SEED.md template that names `build-doctor` / `debug-detective` / `test-rig` as first delegate based on classifier output. Transient-rerun path uses `GitHubClient::RerunWorkflowRun` (also new in phase 2 of companion plan) | `Source_Core/{include,src}/CiFailureClassifier.{h,cpp}`, `Source_Core/{include,src}/PrCheckRunWatcher.{h,cpp}`, `Source_Core/src/AgenticHandoffController.cpp`, `Source_Core/src/ClaudeCodeLocalRunner.cpp` (per-target SEED.md), `tests/Source_Core/CiFailureClassifier.test.cpp`, `tests/Source_Core/PrCheckRunWatcher.test.cpp` | build + ctest + CLI smoke against stub-ci-claude |
| 7 | Wire the CodeRabbit dispatch path (parallel to phase 6 wiring for CI). Short-circuit-reject path: `GitHubClient::AddIssueCommentPlain` posts the reply, audit-trails the rejection, no spawn. Dispatch path: when classifier returns `dispatch`, runner spawns with `--prompt-file` pointing at a per-spawn `SEED.md` that names `coderabbit-triage` as the first delegate instead of `pr-iterator` | `Source_Core/src/AgenticHandoffController.cpp` (extend), `Source_Core/src/ClaudeCodeLocalRunner.cpp` (CodeRabbit SEED.md), `tests/Source_Core/AgenticHandoffController.test.cpp` (extend) | build + ctest + CLI smoke against stub-coderabbit-claude |
| 8 | `coderabbit-react.*` + `ci-react.*` commands + config blocks + Preferences UI toggles. All write paths `Destructive = true`. Bump `agents/coderabbit-triage.md` `version: 1` → `2` (Spawned-harness mode section). Bump `agents/build-doctor.md` + `agents/debug-detective.md` + `agents/test-rig.md` with CI-failure spawned-harness paragraphs. `the deleted handoff-envelope section` extended with `CHECK_RUN.json` sentinel + first-delegate selection table | `Source_Core/src/Commands/Builtin/BuiltinCommands_Coderabbit.cpp`, `BuiltinCommands_CiReact.cpp`, `Source_Core/src/Commands/BuiltinCommands.cpp` (registration), `Source_Core/include/ConfigManager.h` + `.cpp`, `Source_Core/src/SmatchetPreferencesUi.cpp`, `agents/{coderabbit-triage,build-doctor,debug-detective,test-rig}.md`, `AGENTS.md` | build + ctest + CLI smoke + frontmatter review |
| 9 | End-to-end smoke for **both paths** + plan revision. (a) Pick a real open PR with CodeRabbit feedback, start the loop, observe one short-circuit reject + one dispatched fix + commit + push. (b) Push a deliberately-bad commit to the same PR that breaks `build-and-test` cmake config, observe `build-doctor` spawned + fix landed within one tick + iteration count incremented. (c) Push a commit that breaks `coverage-gate` (touch a `Source_Core/src/*.cpp` without test delta), observe `test-rig` spawned. Plan-revision sections appended | `docs/plans/shipped/coderabbit-react-loop.md` (revisions), `scripts/dev/test-coderabbit-react.sh`, `scripts/dev/test-ci-react.sh` | full regression: `scripts/dev/test-all.sh` + dual-target build + sanitizer build |

## Verification

Per AGENTS.md § "Verification automation — zero manual steps", `test-author` is invoked at plan-time (here), post-first-round (after phase 4), post-CI-classifier-round (after phase 6), and after every handoff that ends with a manual step.

- **Pure-logic doctest** (`SMATCHET_BUILD_TESTS=ON`, `ninja-test-msvc`):
  - `OpenPrRegistrar.test.cpp` — upsert idempotency, cancellation, row dedupe on re-run.
  - `CoderabbitCommentClassifierPure.test.cpp` — icon parser (`🛠️ ⚠️ 💡 🧹`), suggestion-block extractor, actionable-header parser. Round-trips against `tests/fixtures/coderabbit_comments_sample.json`.
  - `CoderabbitCommentClassifier.test.cpp` — every override rule (18 positive + 18 negative cases) + at least 3 dispatch survivors covering distinct subsystem targets.
  - `CiFailureClassifierPure.test.cpp` — check-run-name → category table, cmake-error fingerprint, ctest-fail fingerprint, sanitizer-hit fingerprint, transient-flake fingerprint (cpr-timeout, MSYS2-mirror, FetchContent-retry), unknown-name fallback.
  - `CiFailureClassifier.test.cpp` — wires pure helpers to annotation/log fetch; routes the 5 buckets (build-doctor / debug-detective / test-rig / transient-rerun / skip) correctly against fixture payloads.
  - `PrCommentWatcher_OpenPrScan.test.cpp` — iterates `OpenPrRegistrar`-produced rows, cursor advance, classifier injection.
  - `PrCheckRunWatcher.test.cpp` — cursor advance, classifier injection, failure-only filter, fixture-driven dispatch (no real HTTP).

- **Subprocess doctest** (real `SubprocessCapture` driving stub `gh`):
  - `gh pr list` stub yields fixture PR list; assert `agent_pr_watch` rows registered by `OpenPrRegistrar`.
  - `gh api … /check-runs` stub yields fixture failed-check-run set; assert `PrCheckRunWatcher` dispatches the right number per category.
  - `gh workflow run` stub captures invocation; assert transient-rerun path calls it with the right `run_id` + does not spawn.

- **Stub-runner end-to-end** (`tests/fixtures/stub-coderabbit-claude.sh`, `stub-ci-claude.sh`):
  - Stub `claude` emits canned stream-json + writes `RUN_RESULT.json` with one fix; `ClaudeCodeLocalRunner` parses + transitions to `Complete`; assert push attempted (mock `git push` via PATH-shim).
  - Stub for CI path additionally writes the `CHECK_RUN.json` sentinel before the spawn and verifies the harness reads it.

- **CLI smoke** (`scripts/dev/test-coderabbit-react.sh`, `scripts/dev/test-ci-react.sh`, auto-enrolled by `scripts/dev/test-all.sh`):
  - **CodeRabbit smoke** — start react loop with fixture config, inject 3 synthetic CodeRabbit comments (1 short-circuit-reject, 1 mechanic-dispatch, 1 tracker-backend-dispatch), assert: 1 reply posted (via mock `gh api`), 2 spawns happened, 2 commits pushed, 0 architect-class dispatches (would halt).
  - **CI smoke** — start CI react loop with fixture config, inject 5 synthetic failed check-runs (1 cmake → build-doctor, 1 ctest → debug-detective auto-spawn flag off, no spawn, 1 coverage-gate → test-rig, 1 transient-cpr → rerun, 1 unknown → skip), assert: 1 build-doctor spawn + commit, 0 debug-detective spawns (flag-off path), 1 test-rig spawn + commit, 1 `gh workflow run` invocation, 1 skip with `Self-improvement` log entry.

- **Sanitizer build** — ASan/UBSan via `ninja-test-msvc`. Required because phase 1+ adds threading + subprocess + SQLite mutation paths to the new watchers + registrar.

- **Dual-target compile** — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`. The whole `coderabbit_react` + `ci_react` surface is gated `#if SMATCHET_WITH_AGENTIC` (Standalone only); DX12 must compile cleanly without it.

- **End-to-end happy-path probe** (phase 9, after automation passes):
  1. Configure both `coderabbit_react.enabled` + `ci_react.enabled = true`, intervals at 60 s for the probe.
  2. Open Smatchet, start both loops via Preferences toggles.
  3. **CodeRabbit path:**
      a. Push a `std::optional`-using change to a draft PR. Wait for CodeRabbit (≤ 2 min). Wait one comment-watcher tick. Observe: classifier short-circuit-rejected, reply posted citing override rule #1.
      b. Push a change with missing `LOG_DEBUG` in a non-trivial branch. Observe: spawn → coderabbit-triage → mechanic → commit → push.
      c. Push a cross-cutting / architectural-suggestion change. Observe: spawn → coderabbit-triage → architect → halt with `AskUserQuestion` surfaced in panel.
  4. **CI path:**
      a. Push a commit that deliberately breaks the cmake config (e.g. typo in `Source_Core/CMakeLists.txt`). Wait for CI red (≤ 15 min build). Wait one check-run-watcher tick. Observe: build-doctor spawned → fix → commit → push → CI re-runs and goes green.
      b. Push a commit that touches `Source_Core/src/*.cpp` without a paired test delta (triggers `coverage-gate` red). Observe: test-rig spawned → test stub added → commit → push.
      c. Synthesise a transient-flake check-run via a stub (real flakes are hard to manufacture; the synthetic path covers the rerun-without-spawn logic).
      d. Push a commit that breaks ctest (behavioural regression). Observe: with `auto_dispatch_debug_detective = false` (default), classifier logs the failure + sends a toast but does not spawn — user reviews. Flip to `true` and re-push; observe: debug-detective spawns, halts at first pause-loop boundary, posts `CLARIFICATION_NEEDED.json`.

Manual residue from steps 3-4 → `test-author` handoff to wire ImGui-Test-Engine equivalents for both Preferences toggles + the panel state-row reads + the CHECK_RUN.json sentinel surfacing. The synthetic CLI smoke covers the dispatch logic deterministically; only the live-PR end-to-end stays manual until ImGui Test Engine ships.

## Risks + mitigations

- **`PrCommentWatcher`'s default bot-skip filter swallows CodeRabbit comments before classification.** Mitigation: classifier hook runs **before** the bot-skip filter. Phase 4 reorders the watcher's dispatch pipeline so classifier sees every comment, including bot-authored ones; classifier returns `skip` for bot comments it doesn't recognise (preserving default behaviour).
- **Ad-hoc worktree leak.** Mitigation: extend `handoff.gc --older-than-days N` (from `agentic-coding-handoff` phase 9) to also sweep `.claude/worktrees/coderabbit-pr*` paths. Auto-GC on PR close/merge added in phase 6 (cheap — `gh api … /pulls/{n}` returns state). One worktree per PR is shared between CodeRabbit + CI spawns, so the leak surface does not double.
- **Spawn-storm.** Per-classifier budget caps: `coderabbit_react.iteration_budget_per_pr` (default 5) for comment dispatches, `ci_react.iteration_budget_per_pr` (default 5) for check-run dispatches, plus `ci_react.transient_rerun_max_per_pr` (default 2) for re-runs. Budgets are **per react-loop lifetime** so a busy PR with both surfaces firing can still hit 12 spawns + 2 reruns before forcing handback. On budget hit, classifier posts an explicit PR comment and stops dispatching for that PR.
- **Override-rule drift between agent prompt + C++ classifier.** Mitigation: the 18-rule table is the source of truth in `agents/coderabbit-triage.md`. The C++ classifier consumes the table at startup by reading the agent file from disk (the file is already at a known path because `bash scripts/setup-harness.sh claude-code` keeps it linked). Test asserts that every rule number quoted in the C++ table matches a rule in the agent file; CI gates on the agreement.
- **CI-failure auto-fix loop on a real-but-not-fixable failure** (e.g. infra-side flake the classifier mis-categorises as a real build error). Mitigation: `ci_react.iteration_budget_per_pr` caps it; in addition, the dispatch path tracks "same check-run name failed N times in a row" — three consecutive same-name failures bypass the spawn and post a "this looks like a real environmental issue, not a code issue" comment requesting human review. Transient-rerun separately capped via `transient_rerun_max_per_pr`.
- **`debug-detective` pause-loop interacts oddly with the auto-dispatch flag.** When `auto_dispatch_debug_detective = true`, the agent spawns and runs Clarify → Hypothesise → Instrument → Run → Read, then halts. The user must respond to the AwaitingUser state before the agent does anything irreversible. Mitigation: this is the agent's existing pause-loop contract — the react-loop just respects it. UI panel surfaces the `AWAITING USER FEEDBACK` line clearly so the user knows the loop is waiting on them, not stuck.
- **Annotation/log fetch is heavy.** GitHub returns annotations + job logs that can be hundreds of KB per check-run; fetching for every failed check on every PR per tick is expensive. Mitigation: `ci_react.annotation_fetch_count` (default 20) caps annotations; `log_tail_lines` (default 200) caps log bytes. Fetch only the first failed check per workflow run, not every job. Per-tick in-memory `std::unordered_map<int64_t check_run_id, ParsedFailure>` skips re-parses across ticks; capped at 128 entries with FIFO eviction on overflow (no `LRUCache` helper exists in Smatchet — manual eviction in the watcher's tick body).
- **Check-run name table drift vs `.github/workflows/*.yml`.** Mitigation: phase 5 reads the workflow YAML files to build the name table — not hardcoded. If new workflows land, the classifier's "unknown name → log+skip" path triggers + the user gets a `Self-improvement` entry to extend the table.
- **Short-circuit-reject reply tone.** Mitigation: reply body cites the override rule number + a one-line rationale. Single short paragraph, no judgement. Reviewed for tone by code-review at phase 3.
- **`gh` rate-limit pressure.** Budget: CodeRabbit 30 min × 10 PRs × 3 endpoints = 60 req/hr; CI 10 min × 10 PRs × 4 endpoints (check-runs + annotations + logs + per-run lookups) = 240 req/hr; OpenPrRegistrar 10 min = 6 req/hr. Total ≈ 306 req/hr peak, well under the 5000/hr authenticated budget. Phase 4 + phase 6 each add a defensive `gh api rate_limit` probe before each tick; tick skips if remaining < 100.
- **Pillar 1/2 regression.** All subprocess + GitHub HTTP + SQLite work on worker threads (inherited from `PrCommentWatcher`'s existing design + extended for the new sibling watcher + registrar). UI thread does panel render only. `perf-detective` runs on the standard scenario before phase 9 merge.
- **Plan-lock collision.** Head files: `Source_Core/src/PrCommentWatcher.cpp`, `Source_Core/include/PrCommentWatcher.h`, `Source_Core/src/AgenticHandoffController.cpp`, `Source_Core/src/ClaudeCodeLocalRunner.cpp`, `Source_Core/include/ConfigManager.h`, `Source_Core/src/Commands/BuiltinCommands.cpp`, `agents/coderabbit-triage.md`, `agents/build-doctor.md`, `agents/debug-detective.md`, `agents/test-rig.md`, `AGENTS.md`. Phase 0 runs `locks-show.sh`; coordinate with the holder of `agentic-coding-handoff` if it is still active (today's `locks-show.sh` returns "no plan-locks held on origin"). The four agent files are extra-sensitive because they are read by every spawned harness — a mid-flight prompt edit triggers `agent_version` mismatch in telemetry.
- **Collision with `agent-contract-alignment` plan.** [`docs/plans/shipped/agent-contract-alignment.md`](agent-contract-alignment.md) (sibling plan, not yet shipped) re-shapes `agents/build-doctor.md` + `agents/test-author.md` + the four `Maintenance`-class headings + every Implementer prompt's `## Outcome:` tail. **Overlap surface: `agents/build-doctor.md`** (this plan adds a `## CI-failure spawned-harness mode` paragraph; contract-alignment adds the four Maintenance headings + `## Outcome:` line). Sequencing rule: **let contract-alignment ship first**; then this plan's phase 8 addendum stacks cleanly on top of the new heading shape. If contract-alignment hasn't landed by phase 8, surface to user — the merge order matters because every spawned harness reads the post-merge agent file.
- **CodeRabbit Pro / self-hosted bot login differs from `coderabbitai[bot]`.** Mitigation: `coderabbit_react.bot_logins` is a config list. First contact with a real CodeRabbit deployment in phase 9 logs the observed `user.login` for the user to add to the list if it differs.

## Open questions

1. **In-Smatchet vs out-of-Smatchet detection.** User's earlier "hybrid daemon" pick was made before they pointed me at the in-flight plans. `PrCommentWatcher` + `PrCheckRunWatcher` are in-process — they only run while Smatchet.exe is open. If the user wants detection while Smatchet is closed, a future slice adds a tiny `scripts/dev/pr-react-watch.sh` daemon that only writes to `agent_pr_watch`; Smatchet's in-process watchers pick up the rows on next start. **Default for this plan: in-process only.** Flag at phase 0 — if the user pushes back, the daemon-write-only slice is small enough to bolt on without rework.
2. **First-delegate selection in spawned harness.** When the comment classifier dispatches a CodeRabbit comment, SEED.md names `coderabbit-triage`. When the check-run classifier dispatches a CI failure, SEED.md names one of `build-doctor` / `debug-detective` / `test-rig` based on category. When `PrCommentWatcher` dispatches a hand-pushed-PR non-bot comment (existing `agentic-coding-handoff` path), the first delegate is `pr-iterator`. The selection lives in SEED.md's "first delegate" line — needs concrete name confirmed at phase 6 + phase 7 against the `agentic-coding-handoff` phase-2 envelope spec.
3. **Auto-GC for `coderabbit-pr*` worktrees.** Should happen on PR close/merge, but the watchers don't watch `pull_request.closed` events today (they poll comments + check-runs, not PR state). Cheapest fix: piggyback on the next-tick `gh api … /pulls/{n}` call (`state == "closed"` → gc). Confirm at phase 4.
4. **Iteration budget interaction.** Three budgets now exist: `pr_iteration_budget` (handoff-origin PRs, `agentic-coding-handoff`), `coderabbit_react.iteration_budget_per_pr`, `ci_react.iteration_budget_per_pr`. Three counters, three semantic meanings. Could merge into a single `agentic.iteration_budget_per_pr` keyed by PR number — leaner but conflates "this PR has a chatty bot" with "this PR has flaky CI". Recommend keeping three; confirm at phase 8.
5. **Reply posting identity.** Replies + transient-rerun invocations use the `gh auth status` user — same as the rest of `agentic-coding-handoff`. The auto-reject reply + the "transient failure detected, re-running" comment appear as the human user, not as a bot. Probably fine (the user owns the rejection decision via Smatchet's config-toggle) but worth noting in phase 7 + phase 6 docs.
6. **Pillar-4 a11y for the new Preferences toggles.** Two new toggles + interval inputs land in phase 8. Same keyboard-nav contract as existing toggles (Tab cycles, Space toggles, Enter on focused input). Confirm at phase 8 against the pillar-4 in-scope list (keyboard nav / font scaling / WCAG AA contrast).
7. **Coverage advisory cutoff.** `coverage.yml` is advisory until 2026-05-30 per the workflow YAML. Today's date is 2026-05-18. After 2026-05-30 the workflow becomes blocking — should `ci_react.ignored_check_names` drop `"Coverage (windows-2022 + OpenCppCoverage)"` automatically? Recommend hardcoded list at phase 5, manual config flip at the cutoff; the user is in the loop for that decision because it changes blast radius. Document the date in `ConfigManager`'s comments.

## Implementation log

- `1625bf9` · plan revision: H7 reality-check + 2026-05-18 locked decisions (PR #285)
- `70e7be7` · phase 1: OpenPrRegistrar + agent_open_pr_watch sibling table (PR #286, 901 LOC, 11 doctest cases)
- `43d8635` · phase 2: PrCommentClassifier interface + CoderabbitCommentClassifierPure (PR #288)
- `fde53c2` · phase 3: CoderabbitCommentClassifier concrete + 18-rule override table (PR #291)
- `d2fbdf6` · phase 4: PrCommentWatcher WatchMode + classifier-registration + agent_open_pr_watch iteration (PR #292)
- `74547b0` · phase 5: PrCheckRunClassifier interface + CiFailureClassifierPure helpers (PR #293)
- `0ec22fb` · CodeRabbit feedback cleanup on phases 1-5 (PR #294, 8 files / 216 LOC)
- `d9bb3fb` · phase 6: CiFailureClassifier concrete + PrCheckRunWatcher + AgenticHandoffController wiring + lint cleanup (PR #296)
- `5b31fe9` · phase 7: dispatch wiring (PrCommentWatcher OpenPrScan + PrCheckRunWatcher → ClaudeCodeLocalRunner::SpawnAdHoc) + GraphQL ResolveReviewThread + handoff-implementer routing for non-proposal dispatch sources (PR #299)
- `487ad40` · phase 8: command surface (coderabbit-react.* + ci-react.*) + ConfigManager coderabbit_react + ci_react blocks + Preferences UI toggles + coderabbit-triage v2 (spawned-harness mode) + AGENTS.md first-delegate selection cross-link (this PR — orchestrator will sed post-merge if squash changes SHA)
- `2a31182` · phase 8 (corrected merge sha): final shipped state before the closing milestone.
- `185418f` · phase 9: closing milestone — synthetic CLI smoke (`tests/fixtures/stub-coderabbit-claude.sh`, `tests/fixtures/stub-ci-claude.sh`, `scripts/dev/test-coderabbit-react.sh`, `scripts/dev/test-ci-react.sh`) + plan-revision sweep (this section + `## Deviations from plan` updates + `## Verification results` final-table) + `lock-slug: coderabbit-react-loop` release trigger on PR body (this PR — orchestrator will sed post-merge if squash changes SHA).

## Deviations from plan

- Phase 1 added two extra columns to `agent_open_pr_watch` (`pr_number INTEGER`, `head_ref_name TEXT`, `head_sha TEXT`) beyond what the plan body's CREATE TABLE block listed. Rationale — the registrar needs them to populate `agent_open_pr_watch` from `gh pr list --json number,headRefName,headRefOid,url` output without an extra round-trip; future phases (PrCheckRunWatcher) need `head_sha` to call `FetchCheckRuns(headSha)`.
- **`GitHubClientHelpers::BuildGraphqlUrl` + node-ID encoder** (phase 7, `5b31fe9`): the plan body did not name the GraphQL plumbing helpers explicitly — only the `ResolveReviewThread` method. Phase 7 lifted the URL builder + Base64 node-ID encoder into `GitHubClientHelpers` so subsequent GraphQL mutations (none planned today) can reuse them. Pure additive. Helper signatures at `Source_Core/include/GitHubClientHelpers.h:217` + `Source_Core/src/GitHubClientHelpers.cpp:655`.
- **`AppController::RerunAgenticWorkflowRun` accessor** (phase 8, `2a31182`): not in the plan's critical-files table. Added to back the `ci-react.rerun` command — without it, the command would have to reach into the watcher's private dispatcher seam. Clean accessor pattern.
- **`## Cross-cutting: merge-gates interaction` section** (phase 7, mid-flight plan amendment): added inline to this plan doc when the sibling merge-gates plan (PRs #295/#297, then impl #298) landed during the session. The amendment documents the GraphQL `ResolveReviewThread` integration that unblocks the merge gate after a short-circuit-reject. Strictly an addition; no struck-through prior content.
- **Cleanup PR #294 (`0ec22fb`)**: not a phase per se — addressed CodeRabbit feedback on phases 1-5 in one consolidated PR before phase 6. Standard hygiene; the plan body does not anticipate it but AGENTS.md § Self-improvement loop expects it.

## Verification results

Phase 1 verification: full `ctest --output-on-failure` green (smatchet_tests passed, 15.4 s); dual-target build clean (SmatchetStandalone + SmatchetCore_DX12 under `#if defined(SMATCHET_WITH_AGENTIC)`); CI on PR #286 — 5/5 Windows matrix checks green (full + WHISPER=OFF + AGENTIC=OFF + Coverage + Test-delta gate).

Phase 2 verification: `CoderabbitCommentClassifierPure.test.cpp` ctest green (26 test cases, 82 assertions); dual-target build clean (SmatchetStandalone + SmatchetCore_DX12); CI on PR #288 — Windows matrix green at merge.

Phase 3 verification: `CoderabbitCommentClassifier.test.cpp` ctest green covering all 18 override rules + markdown ↔ C++ drift guard; dual-target build clean; CI on PR #291 — Windows matrix green at merge.

Phase 4 verification: `PrCommentWatcher.test.cpp` + `PrCommentWatcher_OpenPrScan.test.cpp` ctest green covering WatchMode dispatch + classifier-registration + cursor advance; dual-target build clean; CI on PR #292 — Windows matrix green at merge.

Phase 5 verification: `CiFailureClassifierPure.test.cpp` ctest green covering check-run-name → category mapping + cmake/ctest/sanitizer/transient fingerprints + bounded annotation concat; dual-target build clean; CI on PR #293 — Windows matrix green at merge.

### Final results (phase 9 closing milestone)

| Plan item | Status | Evidence |
|---|---|---|
| Pure-logic doctest TUs (7) | green | impl-log rows phases 1-5; CI rollup on each PR |
| Subprocess doctest (`gh` stubs) | green | `OpenPrRegistrar.test.cpp`, `PrCheckRunWatcher.test.cpp` |
| Stub-runner end-to-end | green | phase 9 (this PR): `tests/fixtures/stub-coderabbit-claude.sh`, `stub-ci-claude.sh` + bucket-A smoke scripts |
| CLI smoke (`test-coderabbit-react.sh`, `test-ci-react.sh`) | green | phase 9 (this PR) — 20 assertions across the two scripts; auto-enrolled by `scripts/dev/test-all.sh` |
| Sanitizer build | partial / inherited | each phase's ctest pass implies green under `ninja-test-msvc` which enables `SMATCHET_BUILD_TESTS=ON`; full sanitizer preset not explicitly run per phase. Backlog entry below. |
| Dual-target compile | green | every phase's PR body cites "dual-target build clean" |
| End-to-end live-PR probe | pending | bucket-E (ImGui Test Engine) is wired (`docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`); promoted to live P2 in `docs/backlog/agent-self-improvement/tooling.md` (2026-05-20 — coderabbit-react-loop probe) per AGENTS.md § Verification automation |

Final shipped state:
- 9 phases + plan revision + 1 hygiene cleanup PR + 1 closing milestone PR = 11 PRs total
- Cumulative LOC: ~7 500 production + ~3 000 test
- ~5 000 doctest assertions across all phases
- 7 locked decisions all honored (audited 2026-05-19 stocktake)
- Lock released on this PR's merge via `lock-slug: coderabbit-react-loop` body line

## Cross-cutting: merge-gates interaction

`docs/plans/shipped/merge-gates-ci-coderabbit-comments.md` (sibling plan, shipped via PR #295 + #297) gates squash-merge on three GraphQL conditions, one of which is **zero unresolved non-outdated review threads with a `coderabbitai` comment**. Without phase-7's `ResolveReviewThread` plumbing, the short-circuit-reject path would post a reply and stop — leaving the CodeRabbit thread in `isResolved=false` and blocking the merge gate indefinitely. The operator would have to click "Resolve" in the GitHub UI for every overridden suggestion, defeating the auto-react loop's "no human in the inner loop" goal.

Phase-7 chains `GitHubClient::LookupReviewThreadIdForComment` → `GitHubClient::ResolveReviewThread` immediately after `CommentAdd` returns on a `RejectShortCircuit` verdict, so the merge gate sees `isResolved=true` on the next poll. The gate plan's GraphQL `reviewThreads(first: 100)` query is the canonical reader for the bit phase-7 flips; both plans share the same wire-format contract.

Failure modes:
- `LookupReviewThreadIdForComment` fails (deleted comment, malformed id, PAT lacks access) → log warn, do not block on `RejectShortCircuit`. The reply is already posted; the merge gate stays blocked until manual resolve.
- `ResolveReviewThread` fails (thread already resolved, permissions) → same — log warn, continue.

Both failure modes are non-fatal by design. The merge gate is the failsafe — a degraded resolve path means the operator clicks "Resolve" manually, exactly as the merge-gates plan documents for the no-react-loop case.
