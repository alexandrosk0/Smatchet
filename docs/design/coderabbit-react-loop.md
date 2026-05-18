# CodeRabbit react loop — slim extension on `agentic-coding-handoff`

> **Slug:** `coderabbit-react-loop`. Sits **after** `docs/design/agentic-triage-flow.md` (phases 0–2) and `docs/design/agentic-coding-handoff.md` (phases 0–7). Do not start phase 1 until both have shipped.

## Context

Earlier this session two new artefacts shipped:
- `.coderabbit.yaml` — baseline CodeRabbit bot config (chill profile, vendored-path filter, `path_instructions` citing C++14-hard / dual-target / `LOG_*` / `TrackerHttpClient` / pillar 2).
- `agents/coderabbit-triage.md` — read-only investigator agent that ingests PR-bot feedback via `gh api`, runs an 18-rule override table to reject invariant-violating suggestions, classifies survivors, and emits handoff packets.

User wants a **local** auto-react loop equivalent to "fix the things CodeRabbit flagged" without a cloud Claude CLI. Initial draft proposed an out-of-Smatchet bash daemon + Windows Task Scheduler + SessionStart-hook surface.

**Rereading the existing design docs invalidates most of that draft.** The two in-flight plans collectively cover the polling, the worktree spawn, the sentinel-file IPC, the iteration loop, the agent file layout — every primitive the daemon shape would have re-implemented. This plan therefore **shrinks to a thin extension** that wires the already-planned `PrCommentWatcher` to CodeRabbit feedback specifically.

### What already exists in the in-flight plans

| Need | Already covered by |
|---|---|
| In-process PR-comment polling thread | `PrCommentWatcher` (`agentic-coding-handoff` phase 7) — polls `GET /repos/{o}/{r}/pulls/{n}/comments` + `GET /repos/{o}/{r}/issues/{n}/comments` every `pr_poll_interval_sec` (default 120 s), SQLite cursor `agent_pr_watch.last_seen_comment_id` |
| Subprocess spawn for local `claude` | `ClaudeCodeLocalRunner` (`agentic-coding-handoff` phase 3) using `SubprocessCapture` lifted from `P4Blame` (phase 1) |
| Worktree creation per task | `ClaudeCodeLocalRunner::CreateWorktree` (`git worktree add .claude/worktrees/agent-<id> -b agent/<id>/<slug> origin/develop`) |
| Sentinel-file IPC (`SEED.json`, `CLARIFICATION_NEEDED.json`, `USER_RESPONSE.json`, `RUN_RESULT.json`, `PR_URL.txt`) | `AGENTS.md § Handoff envelope` (phase 2) |
| Iteration loop (re-spawn harness when new comments arrive) | `PrCommentWatcher` → `runner.Resume(proposalId, formattedComments + diff)` |
| GitHub HTTP wrapper | `GitHubClient` (`agentic-triage-flow` phase 2) — `FetchPrComments`, `AddIssueCommentPlain` |
| Audit trail per state transition | `BackendAuditTrail::AppendEvent` already wired by `AgenticHandoffController` (`agentic-coding-handoff` phase 4) |
| Live-progress channel for the spawned harness | `.progress.log` tail surfaced in `SmatchetAgentHandoffUi` panel (`agentic-coding-handoff` phase 8) |
| PR-iteration agent | `agents/pr-iterator.md` (`agentic-coding-handoff` phase 7) — classifies each unresolved comment (build / test / style / logic / design) and routes |
| CodeRabbit-specific triage + 18-rule override table | `agents/coderabbit-triage.md` (already shipped this session) |
| CodeRabbit bot config | `.coderabbit.yaml` (already shipped this session) |
| In-process budget enforcement | `pr_iteration_budget` in `ConfigManager`, enforced inside `PrCommentWatcher` |
| Iterating-state machine | `HarnessRunState` enum already lists `Iterating` (`agentic-coding-handoff` phase 4) |

### What is **NOT** yet covered (this plan adds)

1. **Open-PR registration scan.** `PrCommentWatcher` only watches PRs in `agent_pr_watch` — populated when `AgenticHandoffController` opens a PR via a handoff. **PRs the user hand-pushed are never watched.** CodeRabbit-react must register every open PR (whether handoff-origin or hand-pushed) so the watcher polls all of them.
2. **Comment-source classifier hook.** `PrCommentWatcher`'s default behaviour is "skip bot comments, dispatch non-bot comments to `pr-iterator`". CodeRabbit feedback is bot-authored — needs an inclusive filter for `user.login == "coderabbitai[bot]"` plus dispatch to `coderabbit-triage` (not `pr-iterator`).
3. **Ad-hoc worktree for non-handoff PRs.** Hand-pushed PRs have no `ClaudeCodeLocalRunner`-owned worktree. Need on-demand worktree creation (`git worktree add .claude/worktrees/coderabbit-pr<N> -b coderabbit/pr<N>/iter<n> origin/<headRefName>`) plus cleanup on PR close/merge.
4. **`coderabbit-triage` v2 — spawned-harness mode.** Today the agent emits handoff packets to a parent orchestrator. When invoked **inside** a spawned `claude` subprocess (no parent orchestrator), it must act as orchestrator itself: run override-rules, dispatch fixes directly to specialists, run the slice-boundary build + test, write `RUN_RESULT.json`, push back to the PR.
5. **Short-circuit reject for invariant violations.** Most CodeRabbit suggestions that violate Smatchet rules (use `std::optional`, etc.) can be rejected **without** spawning a harness — pure C++ classifier + 1-line PR reply via `GitHubClient::AddIssueCommentPlain`. Saves the cost of every full spawn for trivially-rejectable suggestions.
6. **`coderabbit-react.*` command surface + config block** alongside the existing `handoff.*` commands.

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
│ AgenticHandoffController            │            │ open-PR registration scan        │
│  └─ PrCommentWatcher (poll loop)    │◄───────────┤ (every interval, gh pr list →     │
│       ├─ poll → fetch new comments  │            │  agent_pr_watch upsert)          │
│       ├─ classify → dispatch        │◄───────────┤ PrCommentClassifier interface +  │
│       └─ runner.Resume(...)         │            │  CoderabbitCommentClassifier      │
└─────────────────────────────────────┘            └──────────────────────────────────┘
                  │
                  │ classifier == coderabbit
                  ▼
┌─────────────────────────────────────┐            ┌──────────────────────────────────┐
│ ClaudeCodeLocalRunner (already)     │◄───────────┤ ad-hoc worktree creation for     │
│  └─ spawn claude in PR worktree     │            │  hand-pushed PRs (no handoff     │
│       └─ first delegate =           │◄───────────┤  origin worktree exists)         │
│         agents/coderabbit-triage    │            │                                  │
│         (v2 — spawned-harness mode) │            │ ConfigManager.coderabbit_react   │
└─────────────────────────────────────┘            │  block + Preferences toggle      │
                  │                                └──────────────────────────────────┘
                  │ short-circuit (override-rule rejection)
                  ▼
              GitHub PR reply (no spawn cost)
```

**No new daemon, no new SessionStart hook, no new bash watcher script** — `PrCommentWatcher` already covers in-process polling on a `std::thread`. The earlier draft's `scripts/dev/coderabbit-watch.sh` + `start-coderabbit-watch.ps1` + Windows Task Scheduler approach is **abandoned** as duplicate functionality.

## Critical files

### New
| Path | Purpose |
|---|---|
| `Source_Core/include/PrCommentClassifier.h` | Abstract interface — `virtual ClassificationResult Classify(const PrComment&)` returns `{dispatch | reject_short_circuit | skip}` + target-agent name + optional pre-built short-circuit reply |
| `Source_Core/{include,src}/CoderabbitCommentClassifier.{h,cpp}` | Concrete classifier — recognises `user.login == "coderabbitai[bot]"`, applies the 18-rule override table in C++ for short-circuit reject (saves a spawn for "use `std::optional`" suggestions), dispatches survivors to `coderabbit-triage` |
| `Source_Core/{include,src}/CoderabbitCommentClassifierPure.{h,cpp}` | Pure helpers — body parser for CodeRabbit's `🛠️ ⚠️ 💡 🧹` icons, `_Actionable comments posted: N_` header parser, `suggestion` block extractor. Pure-helper TU split per AGENTS.md § "Pure-helper TU-split recipe" so doctest rig can exercise without GitHub HTTP |
| `Source_Core/src/Commands/Builtin/BuiltinCommands_Coderabbit.cpp` | `coderabbit-react.start`, `.stop`, `.status`, `coderabbit-react.poll-now <pr>` (manual fire) |
| `tests/Source_Core/CoderabbitCommentClassifierPure.test.cpp` | Override-rule table coverage (each of the 18 rules has at least one positive + one negative case), icon parser, suggestion-block extractor |
| `tests/Source_Core/PrCommentWatcher_OpenPrScan.test.cpp` | New TU for open-PR-scan mode (separate from the existing `PrCommentWatcher.test.cpp` so the two modes have independent failure footprints) |
| `tests/fixtures/coderabbit_comments_sample.json` | Recorded `gh api … /pulls/{n}/comments` payload featuring at least one of each: actionable suggestion that should dispatch, nitpick that should dispatch via mechanic, suggestion that should short-circuit reject per override rule #1 |
| `tests/fixtures/stub-coderabbit-claude.sh` | Bash script masquerading as `claude` for the spawned-harness-mode test — emits canned stream-json + writes `RUN_RESULT.json` with one fix applied |
| `scripts/dev/test-coderabbit-react.sh` | Bucket-A CLI smoke — starts the react loop, injects fixture comments, asserts the watcher dispatches + the stub claude makes a fix + the loop pushes |

### Modified (existing)
| Path | Change |
|---|---|
| `Source_Core/include/PrCommentWatcher.h` | Add `enum class WatchMode { OriginTracking, OpenPrScan }`; add classifier-registration API `void RegisterClassifier(std::unique_ptr<PrCommentClassifier>)`; default behaviour preserved when no classifier registered |
| `Source_Core/src/PrCommentWatcher.cpp` | Add open-PR-scan mode body (every `Tick`, in `OpenPrScan` mode, `gh pr list --base develop --state open --json number,headRefName` and upsert each row into `agent_pr_watch` with `origin = "open-pr-scan"`); inject classifier in the dispatch path |
| `Source_Core/src/AgenticHandoffController.cpp` | Register `CoderabbitCommentClassifier` at startup when `coderabbit_react.enabled = true`; gate by `#if SMATCHET_WITH_AI` |
| `Source_Core/src/ClaudeCodeLocalRunner.cpp` | Add ad-hoc worktree path for PRs without handoff-origin worktree (resolved by lookup in `agent_pr_watch.origin` — if `"open-pr-scan"`, create `coderabbit-pr<N>/iter<n>` worktree off `origin/<headRefName>`) |
| `Source_Core/include/ConfigManager.h` + `.cpp` | New block (no schema-version bump — held until phase 6 verification per AGENTS.md § "Schema-version bumps"): |
| `Source_Core/src/SmatchetPreferencesUi.cpp` | New "CodeRabbit react loop" toggle row + interval input |
| `agents/coderabbit-triage.md` | Bump `version: 1` → `2`. Add `## Spawned-harness mode` section: when invoked inside a spawned `claude` subprocess with no parent orchestrator (detected by `SEED.json` absence + `pr-iteration-mode` env var presence), act as orchestrator — execute the routed fix as `Edit` calls, run slice-boundary `cmake --build` + `scripts/dev/test-all.sh`, write `RUN_RESULT.json`, exit with success. Outside spawned-harness mode (existing behaviour), emit handoff packets to parent orchestrator unchanged |
| `AGENTS.md` | Small addendum to § Handoff envelope: `pr-iterator` is the **default** first-delegate inside a spawned harness; `coderabbit-triage` is the alternate when `PrCommentClassifier` routes the comment to it. One-paragraph addition, no new section |
| `.gitignore` | Add `.claude/worktrees/coderabbit-pr*` (the ad-hoc worktree namespace) |

### `ConfigManager` `coderabbit_react` block
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
  }
}
```

## Reuse (do not re-implement)

| Need | Use |
|---|---|
| In-process poll loop | `PrCommentWatcher::Tick` (`agentic-coding-handoff` phase 7) |
| Cursor persistence | `agent_pr_watch.last_seen_comment_id` column (already in `agentic-coding-handoff` schema) — add `origin TEXT` column in this plan to distinguish handoff-origin vs open-PR-scan rows |
| Subprocess spawn | `SubprocessCapture::Run` (`agentic-coding-handoff` phase 1) |
| Worktree creation | `ClaudeCodeLocalRunner::CreateWorktree` (`agentic-coding-handoff` phase 3) — extend to take a `WorktreeOrigin` enum |
| Sentinel-file IPC | `AGENTS.md § Handoff envelope` (`agentic-coding-handoff` phase 2) |
| GitHub HTTP — PR comments | `GitHubClient::FetchPrComments` (`agentic-triage-flow` phase 2 + `agentic-coding-handoff` phase 7) |
| GitHub HTTP — reply to comment | `GitHubClient::AddIssueCommentPlain` (`agentic-triage-flow` phase 2) — also used by short-circuit-reject path |
| Audit trail | `BackendAuditTrail::AppendEvent` (existing) |
| Worker thread + cancel atomic | `AppController::LaunchBackgroundTask` (existing) |
| Worker → UI hand-off | `MainThreadDispatcher::PostToMainThread` (existing) |
| Toast | `SmatchetToastManager::Push` (existing) |
| Override-rule table | `agents/coderabbit-triage.md` § "Override rules" (18 entries, shipped — referenced by `CoderabbitCommentClassifier::ShouldShortCircuitReject` in C++) |
| Routing table | `agents/coderabbit-triage.md` § "Routing table" (referenced for the spawned-harness dispatch) |
| `gh pr list --json` query shape | `agents/git-janitor.md` pre-flight (existing pattern) |
| Plan-lock pre-flight | `bash scripts/dev/locks-show.sh` (existing) |

## Out of scope (deferred or covered elsewhere)

- **Out-of-Smatchet daemon / Task Scheduler.** Abandoned — `PrCommentWatcher` in-process polling makes it redundant for any flow where Smatchet.exe is running. If the user wants detection while Smatchet is closed, a future slice can add an out-of-process `scripts/dev/coderabbit-watch.sh` that only writes to `agent_pr_watch` and lets Smatchet's in-process watcher pick up the row on next start. **Not in this plan.**
- **SessionStart hook in `.claude/hooks/`.** Abandoned for the same reason — `PrCommentWatcher` already surfaces new comments via `SmatchetAgentHandoffUi` while Smatchet runs.
- **PR thread-resolve via GraphQL.** Same scope as `agentic-coding-handoff`'s out-of-scope — separate slice.
- **Cloud Claude execution.** User explicitly rejected; `ClaudeCodeLocalRunner` is the only runner this plan uses (`agentic-coding-handoff`'s `ICodingHarnessRunner` interface allows future cloud runners but they're not wired here).
- **Cross-repo PRs.** Same scope as `agentic-coding-handoff` — only PRs in the current Smatchet repo are watched.
- **CI-failure auto-fix.** The same `PrCommentWatcher` + classifier machinery could later host a `CiFailureClassifier` that watches `pull_request.check_run` events and routes to `build-doctor` / `debug-detective`. **Not in this plan** — flagged as a natural follow-up because it shares 95% of the plumbing.
- **`.coderabbit.yaml` schema changes.** Already shipped; only revisit if a phase uncovers a missing `path_instructions` entry.

## Phased rollout

Hard dependencies: `agentic-triage-flow` phases 0–2 (`GitHubClient` read+write) and `agentic-coding-handoff` phases 0–7 (`PrCommentWatcher` + `ClaudeCodeLocalRunner` + sentinel-file protocol + `pr-iterator`) must have shipped. Plan-lock claim slug: `coderabbit-react-loop`. Run `bash scripts/dev/locks-show.sh` before phase 1; `AgenticHandoffController.cpp` + `PrCommentWatcher.cpp` + `AGENTS.md` are head files — coordinate with whoever holds those if active.

| Phase | Scope | Key files | Gate |
|---|---|---|---|
| 0 | Plan-lock claim, doc move to canonical (`docs/design/coderabbit-react-loop.md`), ADR for the open-PR-scan mode addition (`docs/adr/00NN-prcommentwatcher-open-pr-scan-mode.md`), glossary additions, dependency-shipped check (`gh pr view` for the `agentic-coding-handoff` phase-7 PR, must be merged) | `docs/design/coderabbit-react-loop.md`, `docs/adr/00NN-…md`, `docs/CONTEXT.md` | doc-only |
| 1 | `PrCommentClassifier` interface + `CoderabbitCommentClassifierPure` (icon / suggestion-block / actionable-header parsers — pure helpers in their own TU per AGENTS.md "Pure-helper TU-split recipe"). Doctest covers every CodeRabbit comment shape from the recorded fixture | `Source_Core/include/PrCommentClassifier.h`, `Source_Core/{include,src}/CoderabbitCommentClassifierPure.{h,cpp}`, `tests/Source_Core/CoderabbitCommentClassifierPure.test.cpp`, `tests/fixtures/coderabbit_comments_sample.json` | dual-target build + ctest |
| 2 | `CoderabbitCommentClassifier` concrete impl — wires the pure helpers to the 18-rule override table; for each rule that fires, builds a short-circuit-reject reply body that cites the rule number. Doctest covers each of the 18 rules + at least one survivor that dispatches to `coderabbit-triage` | `Source_Core/{include,src}/CoderabbitCommentClassifier.{h,cpp}`, `tests/Source_Core/CoderabbitCommentClassifier.test.cpp` (extends fixture) | build + ctest |
| 3 | Extend `PrCommentWatcher` with `WatchMode` enum + classifier-registration API. Default `OriginTracking` behaviour unchanged. New `OpenPrScan` mode body: every `Tick`, `gh pr list --base develop --state open --json number,headRefName` (subprocess via `SubprocessCapture`), upsert rows into `agent_pr_watch` with `origin = "open-pr-scan"`. Add `origin TEXT` column to the `agent_pr_watch` schema (additive — no version bump per AGENTS.md "Schema-version bumps") | `Source_Core/include/PrCommentWatcher.h`, `Source_Core/src/PrCommentWatcher.cpp`, `tests/Source_Core/PrCommentWatcher_OpenPrScan.test.cpp` | build + ctest |
| 4 | Wire the dispatch path: `CoderabbitCommentClassifier` registered in `AgenticHandoffController` when `coderabbit_react.enabled = true`. Short-circuit-reject path: `GitHubClient::AddIssueCommentPlain` posts the reply, audit-trails the rejection, no spawn. Dispatch path: when classifier returns `dispatch`, runner spawns with `--prompt-file` pointing at a per-spawn `SEED.md` that names `coderabbit-triage` as the first delegate instead of `pr-iterator` | `Source_Core/src/AgenticHandoffController.cpp`, `Source_Core/src/ClaudeCodeLocalRunner.cpp` (ad-hoc worktree path for `origin = "open-pr-scan"`), `tests/Source_Core/AgenticHandoffController.test.cpp` (extend) | build + ctest + CLI smoke against stub-coderabbit-claude |
| 5 | `coderabbit-react.start --interval-sec N` / `.stop` / `.status` / `.poll-now <pr>` commands + config block + Preferences UI toggle. All write paths `Destructive = true`. Bump `agents/coderabbit-triage.md` `version: 1` → `2` with new `## Spawned-harness mode` section | `Source_Core/src/Commands/Builtin/BuiltinCommands_Coderabbit.cpp`, `Source_Core/src/Commands/BuiltinCommands.cpp` (registration), `Source_Core/include/ConfigManager.h` + `.cpp`, `Source_Core/src/SmatchetPreferencesUi.cpp`, `agents/coderabbit-triage.md`, `AGENTS.md` (one-paragraph addendum) | build + ctest + CLI smoke + frontmatter review |
| 6 | End-to-end smoke + plan revision. Pick a real open PR with CodeRabbit feedback, start the react loop, observe one short-circuit reject + one dispatched fix + commit + push end-to-end. Plan-revision sections appended (`## Implementation log`, `## Deviations from plan`, `## Verification`) | `docs/design/coderabbit-react-loop.md` (revisions), `scripts/dev/test-coderabbit-react.sh` (CLI smoke wraps the synthetic version of the above) | full regression: `scripts/dev/test-all.sh` + dual-target build + sanitizer build |

## Verification

Per AGENTS.md § "Verification automation — zero manual steps", `test-author` is invoked at plan-time (here), post-first-round (after phase 4), and after every handoff that ends with a manual step.

- **Pure-logic doctest** (`SMATCHET_BUILD_TESTS=ON`, `ninja-test-msys2`):
  - `CoderabbitCommentClassifierPure.test.cpp` — icon parser (`🛠️ ⚠️ 💡 🧹`), suggestion-block extractor, actionable-header parser. Round-trips against `tests/fixtures/coderabbit_comments_sample.json`.
  - `CoderabbitCommentClassifier.test.cpp` — every override rule (18 positive + 18 negative cases) + at least 3 dispatch survivors covering distinct subsystem targets.
  - `PrCommentWatcher_OpenPrScan.test.cpp` — upsert idempotency, cursor advance, classifier injection.

- **Subprocess doctest** (real `SubprocessCapture` driving stub `gh`):
  - `gh pr list` stub yields fixture PR list; assert `agent_pr_watch` rows.

- **Stub-runner end-to-end** (`tests/fixtures/stub-coderabbit-claude.sh`):
  - Stub `claude` emits canned stream-json + writes `RUN_RESULT.json` with one fix; `ClaudeCodeLocalRunner` parses + transitions to `Complete`; assert push attempted (mock `git push` via PATH-shim).

- **CLI smoke** (`scripts/dev/test-coderabbit-react.sh`, auto-enrolled by `scripts/dev/test-all.sh`):
  - Start react loop with fixture config, inject 3 synthetic CodeRabbit comments (1 short-circuit-reject, 1 mechanic-dispatch, 1 tracker-backend-dispatch), assert: 1 reply posted (via mock `gh api`), 2 spawns happened, 2 commits pushed, 0 architect-class dispatches (would halt).

- **Sanitizer build** — ASan/UBSan via `ninja-test-msys2`. Required because phase 3+ adds threading + subprocess + SQLite mutation paths to `PrCommentWatcher`.

- **Dual-target compile** — `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`. The whole `coderabbit_react` surface is gated `#if SMATCHET_WITH_AI` (Standalone only); DX12 must compile cleanly without it.

- **End-to-end happy-path probe** (phase 6, after automation passes):
  1. Configure `coderabbit_react.enabled = true`, `poll_interval_sec = 60` (fast for the probe).
  2. Open Smatchet, start the react loop via Preferences toggle.
  3. Push a deliberately-flagged change to a draft PR (e.g. add a function using `std::optional`).
  4. Wait for CodeRabbit to comment (≤ 2 min after push, observed empirically).
  5. Wait one watcher tick. Observe in `SmatchetAgentHandoffUi`: PR registered, classifier short-circuit-rejected the suggestion, reply posted on the PR thread.
  6. Push a second change with a real issue (e.g. missing `LOG_DEBUG` in a non-trivial branch). Observe: spawn happens, fix lands as a commit, PR updated.
  7. Push a third change with a cross-cutting issue (e.g. CodeRabbit suggests an architectural change). Observe: classifier dispatches to `coderabbit-triage`, agent routes to `architect`, handoff halts for user.

Manual residue from steps 4-7 → `test-author` handoff to wire ImGui-Test-Engine equivalents for the Preferences toggle + the panel state-row reads. The synthetic CLI smoke covers the dispatch logic deterministically; only the live-PR end-to-end stays manual until ImGui Test Engine ships.

## Risks + mitigations

- **`PrCommentWatcher`'s default bot-skip filter swallows CodeRabbit comments before classification.** Mitigation: classifier hook runs **before** the bot-skip filter. Phase 3 reorders the watcher's dispatch pipeline so classifier sees every comment, including bot-authored ones; classifier returns `skip` for bot comments it doesn't recognise (preserving default behaviour).
- **Ad-hoc worktree leak.** Mitigation: extend `handoff.gc --older-than-days N` (from `agentic-coding-handoff` phase 9) to also sweep `.claude/worktrees/coderabbit-pr*` paths. Auto-GC of `coderabbit-pr*` worktrees on PR close/merge added in phase 4 (cheap — `gh api … /pulls/{n}` returns state).
- **Spawn-storm if CodeRabbit posts many comments in burst.** Mitigation: `iteration_budget_per_pr` (default 5) caps total spawns per PR per react-loop lifetime. Budget exhausted → reply on PR ("budget exhausted, hand-off to human") + stop dispatching for that PR.
- **Override-rule drift between agent prompt + C++ classifier.** Mitigation: the 18-rule table is the source of truth in `agents/coderabbit-triage.md`. The C++ classifier consumes the table at startup by reading the agent file from disk (the file is already at a known path because `bash scripts/setup-harness.sh claude-code` keeps it linked). Test asserts that every rule number quoted in the C++ table matches a rule in the agent file; CI gates on the agreement.
- **Short-circuit-reject reply tone.** Mitigation: reply body cites the override rule number + a one-line rationale. Single short paragraph, no judgement. Reviewed for tone by code-review at phase 2.
- **`gh` rate-limit pressure at 30-min poll × N open PRs.** Mitigation: 30-min interval × 10 PRs × 3 endpoints = 60 req/hour, well under the 5000/hr authenticated budget. Phase 3 adds a defensive `gh api rate_limit` probe before each tick; tick skips if remaining < 100.
- **Pillar 1/2 regression.** All subprocess + GitHub HTTP + SQLite work on worker threads (inherited from `PrCommentWatcher`'s existing design). UI thread does panel render only. `perf-detective` runs on the standard scenario before phase 6 merge.
- **Plan-lock collision.** Head files: `Source_Core/src/PrCommentWatcher.cpp`, `Source_Core/include/PrCommentWatcher.h`, `Source_Core/src/AgenticHandoffController.cpp`, `Source_Core/src/ClaudeCodeLocalRunner.cpp`, `Source_Core/include/ConfigManager.h`, `Source_Core/src/Commands/BuiltinCommands.cpp`, `agents/coderabbit-triage.md`, `AGENTS.md`. Phase 0 runs `locks-show.sh`; coordinate with the holder of `agentic-coding-handoff` if it is still active.
- **CodeRabbit Pro / self-hosted bot login differs from `coderabbitai[bot]`.** Mitigation: `coderabbit_react.bot_logins` is a config list. First contact with a real CodeRabbit deployment in phase 6 logs the observed `user.login` for the user to add to the list if it differs.

## Open questions

1. **In-Smatchet vs out-of-Smatchet detection.** User's earlier "hybrid daemon" pick was made before they pointed me at the in-flight plans. `PrCommentWatcher` is in-process — it only runs while Smatchet.exe is open. If the user wants detection while Smatchet is closed, a future slice adds a tiny `scripts/dev/coderabbit-watch.sh` daemon that only writes to `agent_pr_watch`; Smatchet's in-process watcher picks up the row on next start. **Default for this plan: in-process only.** Flag at phase 0 — if the user pushes back, the daemon-write-only slice is small enough to bolt on without rework.
2. **`pr-iterator` vs `coderabbit-triage` first-delegate selection in spawned harness.** When the classifier dispatches a CodeRabbit comment, the spawned `claude` reads `SEED.md` whose first delegate is `coderabbit-triage`. When `PrCommentWatcher` dispatches a hand-pushed-PR comment (non-bot), the first delegate is `pr-iterator`. The selection lives in `SEED.md`'s "first delegate" line — needs concrete name confirmed at phase 4 against the `agentic-coding-handoff` phase-2 envelope spec.
3. **Auto-GC for `coderabbit-pr*` worktrees.** Should happen on PR close/merge, but `PrCommentWatcher` doesn't watch `pull_request.closed` events today (it polls comments, not PR state). Cheapest fix: piggyback on the next-tick `gh api … /pulls/{n}` call (`state == "closed"` → gc). Confirm at phase 3.
4. **Iteration budget interaction with `pr_iteration_budget` from `agentic-coding-handoff`.** The original budget caps handoff-PR iterations; this plan adds `iteration_budget_per_pr` for react-loop PRs. Two budgets, two semantic meanings. Could merge into one budget per PR regardless of origin — leaner. Confirm at phase 5.
5. **Reply posting identity.** Replies use the `gh auth status` user — same as the rest of `agentic-coding-handoff`. The auto-reject reply will appear as the human user, not as a bot. Probably fine (the user owns the rejection decision via Smatchet's config-toggle to enable the react loop) but worth noting in phase 4 docs.
