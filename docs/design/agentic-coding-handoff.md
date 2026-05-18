# Agentic Coding Handoff — Plan

> **Slug (canonical):** `agentic-coding-handoff`.
> **Canonical home (post-approval):** `docs/design/agentic-coding-handoff.md`. Per AGENTS.md § "Plan location" and § "Plan-doc safety", once approved the implementer must `git mv` this file (or copy it from `.claude/plans/...`) to `docs/design/agentic-coding-handoff.md` and commit immediately with `wip(plan): agentic-coding-handoff` before any code lands.
> **Companion plan:** `docs/design/agentic-triage-flow.md` (commit `2b84dc1` on `develop`; sibling file once this plan moves to its canonical home). That plan produces approved `AgentProposal` rows; this plan consumes them.

## Context

The companion plan (`agentic-triage-flow`) ends at a human-approved `AgentProposal` in the SQLite proposal store. This plan begins there: when a proposal is approved, Smatchet must drive the work end-to-end by spawning an external coding harness (Claude Code in `--print` / non-interactive SDK mode) that:

1. Receives a seed packet (proposal payload + originating-issue body + comments + repo-state pointers + the repo's `agents/` tree already exposed via `.claude/agents`).
2. Asks clarifying questions when blocked — both via an in-Smatchet panel **and** via a comment on the originating GitHub issue (dual-channel; whichever the user answers first wins).
3. Implements the change autonomously inside an isolated git worktree.
4. Pushes a feature branch and opens a draft PR via `gh pr create`.
5. After the PR opens, **watches the PR for review comments and iterates autonomously** — pushing additional commits in response to feedback until the PR is merged or the user cancels.

The motivating user need: convert "I have a triaged-and-approved issue" into "I have a PR ready to review" without leaving Smatchet. The harness owns the coding. The human owns the decisions (approval, clarification, PR review/merge).

**Why now:** the foundations exist —
- `P4Blame::RunProcessCapture` is the proven cross-platform subprocess wrapper (`CreateProcessW` on Win, `fork() + execvp` on POSIX, 120 s timeout, 4 MB cap).
- `AppController::LaunchBackgroundTask` is the fire-and-forget worker-thread mechanism.
- `SmatchetToastManager` is the existing transient-notification surface.
- `TicketSyncService::StreamingSyncState` is the canonical FSM-with-worker pattern.
- `scripts/setup-harness.sh claude-code` already wires `.claude/agents` → `agents/` junctions + hooks; the spawned harness sees the full agent tree out of the box.
- `scripts/dev/agent-progress.sh` + `tail-agent.sh` define the `.progress.log` live-progress format the harness emits naturally.
- `BackendAuditTrail::AppendEvent` is the durable event log for every state transition.

Nothing requires inventing new infra — the work is wiring.

## Decisions locked (from user clarifying questions)

| Question | Decision |
|---|---|
| Harness host | **Pluggable runner; Claude Code local subprocess first.** New `ICodingHarnessRunner` interface; phase-1 concrete is `ClaudeCodeLocalRunner` (spawns `claude --print --output-format stream-json …` via the `P4Blame::RunProcessCapture` pattern). Later runners (`ClaudeCodeCloudRunner`, `CodexRunner`, `AiderRunner`) drop in behind the interface without controller changes. |
| Clarification channel | **Dual-channel: in-Smatchet panel + GitHub issue comment.** Harness writes `CLARIFICATION_NEEDED.json` to its worktree (panel reads it) **and** posts a comment on the source GitHub issue tagging the user. User responds in either channel; whichever arrives first becomes `USER_RESPONSE.json` and the harness resumes via SDK `--resume`. |
| Permission mode | **Full-auto.** Harness spawned with `--permission-mode bypassPermissions` (or the SDK equivalent). Edits / Writes / Bash run without per-call user prompts. Blast radius limited by: isolated worktree, branch-not-`develop`, PR-must-be-human-reviewed-before-merge, every action audited. |
| PR-revision loop | **Continuous polling.** After PR opens, `PrCommentWatcher` polls the PR every N min (reuses the `agentic-triage-flow` scheduled-poll pattern). New review comments → harness re-spawned with the comments + PR diff injected into the resume prompt; harness pushes an iteration commit. Loop continues until PR merged, user cancels, or budget exhausted. |
| Agent-tree changes | **Two new agents + one AGENTS.md doc addition.** New `agents/handoff-implementer.md` (Implementer class) — first delegate of the spawned orchestrator, consumes `SEED.json` and routes to subsystem specialists. New `agents/pr-iterator.md` (Maintenance class) — owns the PR-comment iteration cycle. New `AGENTS.md § Handoff envelope` section under § Delegation — describes the `SEED.json` contract so all 22 existing agents share vocabulary. |

## Agent-tree additions

Three additions to the canonical `agents/` tree + `AGENTS.md`. None are required for the C++ feature to compile or run end-to-end with stub fixtures — they ship value only once a real `claude` subprocess is spawned. Treat as **phase 2** for the seed-aware pair (the envelope is defined alongside `SEED.json`) and **phase 7** for the PR-iteration pair.

### `agents/handoff-implementer.md` (new — phase 2)

- **Class:** Implementer (per AGENTS.md § Agent output contract).
- **Frontmatter:** `complexity: medium`, `read-only: false`, `triggers: ["SEED.json present in cwd", "handoff slice", "agent-coding handoff", "from spawned harness seed"]`, `delegates-to:` the full subsystem-specialist row (tracker-backend, grid-engine, offline-sync, command-system, lua-binder, mcp-toolsmith, p4-blame, unreal-bridge, test-rig, mechanic), `harness-hints.claude-code: {model: sonnet, effort: high}`, `version: 1`.
- **Job:** read `SEED.json` from `$PWD`, parse the `proposedAction` + payload, build a one-paragraph internal task description, route to the matching subsystem specialist via the existing delegation table, observe the specialist's `## Outcome:`, emit the standard Implementer report (`## Files changed`, `## Smoke-test result`, `## Manual residue`).
- **Hard rules:** never modifies `SEED.json` / `USER_RESPONSE.json` / `RUN_RESULT.json` (those are controlled by `ClaudeCodeLocalRunner`). On ambiguity, writes `CLARIFICATION_NEEDED.json` with the question and stops — the runner surfaces it to the user. Refuses to push commits or open PRs itself — the subsystem specialist or the harness session owns that step.

### `agents/pr-iterator.md` (new — phase 7)

- **Class:** Maintenance (per AGENTS.md § Agent output contract).
- **Frontmatter:** `complexity: medium`, `read-only: false`, `triggers: ["PR comment iteration", "review-comment respond", "review feedback push", "respond to review comments"]`, `delegates-to: [code-review, build-doctor, debug-detective]`, `harness-hints.claude-code: {model: sonnet, effort: high}`, `version: 1`.
- **Job:** input is a structured payload (the resume prompt the watcher injects) carrying the PR diff + the unresolved review comments. For each comment, classify (build / test / style / logic / design) and route: build → `build-doctor`, logic → `debug-detective`, style/format → `mechanic` or inline, design → reply with a clarifying comment and stop (do not silently rework architecture). Apply changes in the worktree, run the slice-boundary `cmake --build` + `scripts/dev/test-all.sh`, push an amend commit (or new commit per `branch.commit-style` config), surface the resulting commit SHA + summary back to the watcher for an iteration-budget tick.
- **Hard rules:** never closes / reopens / merges the PR — only pushes commits. Refuses to touch commits authored by anyone other than the harness identity (avoid stomping on hand-pushed user fixes). Respects the `pr_iteration_budget`; on budget hit, posts a comment "iteration budget exhausted, handing back to user" and stops.

### `AGENTS.md § Handoff envelope` (new section — phase 2)

New top-level section landing under § Delegation. Three subsections:

1. **`SEED.md` + `SEED.json` shape** — the contract every spawned harness reads on entry. Includes a sample JSON with `proposalId`, `sourceTrackerType`, `sourceIssueKey`, `proposedAction`, `payload`, `rationale`, `issueTitle`, `issueBody`, `issueComments` (array), `repoRoot`, `branchName`, `worktreePath`.
2. **Spawned-orchestrator first-move contract** — verbatim instruction: "if `SEED.json` exists at `$PWD`, your first action is to delegate to `handoff-implementer` with that file as inline context; do not re-read it yourself or improvise routing."
3. **Sentinel-file vocabulary** — names + write-once contracts for `CLARIFICATION_NEEDED.json`, `USER_RESPONSE.json`, `RUN_RESULT.json`, `ERROR.json`, `PR_URL.txt`. Each sentinel: who writes it, who reads it, idempotency rules.

This section is read by both Smatchet-side code (the runner enforces the file contracts) and harness-side agents (they obey the contracts when running inside a spawned worktree). Documentation only — no behaviour change on its own; it makes the contract legible.

## Architecture

```
[agentic-triage-flow ships approved AgentProposal]
                       |
                       v
+-------------------------------------------------------+
|  AgenticHandoffController       (agnostic core, new)  |
|   - proposalStore: AgentProposalStore& (existing)     |
|   - audit: BackendAuditTrail&         (existing)      |
|   - runners: vector<unique_ptr<ICodingHarnessRunner>> |
|   - prWatchers: vector<unique_ptr<PrCommentWatcher>>  |
|   - inFlightRuns: vector<HarnessRunState>             |
|   - cancelToken: atomic<bool>                         |
+-------------------------------------------------------+
                       |
       +---------------+----------------+----------+
       v                                v          v
ICodingHarnessRunner (interface)    PrCommentWatcher   SmatchetAgentHandoffUi
       |                                |          |
       v                                v          v
  +-----------------+              GitHub API   ImGui modeless panel
  | ClaudeCodeLocal | (phase 1)    (poll)       (in-flight rows +
  | Runner          |                            clarification box +
  +-----------------+                            PR link)
       |
       v
  SubprocessCapture (lifted from P4Blame)
       |
       v
  child: claude --print ... in .claude/worktrees/agent-<id>/
       writes:  .progress.log
                CLARIFICATION_NEEDED.json (when blocked)
                RUN_RESULT.json (on completion)
                ERROR.json (on failure)
                PR_URL.txt (after gh pr create)
       reads:   SEED.md, SEED.json
                USER_RESPONSE.json (when present)
```

### State machine — `HarnessRunState`

```
Pending → Spawning → Running ┬─→ AwaitingUser ──(answer)──→ Running
                             ├─→ PrOpen ──(comment)──→ Iterating ──(push)──→ PrOpen
                             ├─→ Complete (PR merged or user marks done)
                             ├─→ Failed
                             └─→ Cancelled
```

Atomics + a mutex-protected detail struct, mirroring `TicketSyncService::StreamingSyncState`. UI polls per-frame; worker writes atomics + appends to `eventLog`.

### Per-component sketch

**`SubprocessCapture`** — `Source_Core/{include,src}/SubprocessCapture.{h,cpp}` (new; **lifted from `P4Blame.cpp` lines 81–331**).
- Public API: `SubprocessRunSpec` (argv, workdir, env-vars, timeoutMs, byteCap, optional `stdoutLineSink`, optional `cancel: atomic<bool>*`) → `SubprocessRunResult` (exitCode, stdout, stderr, durationMs, timedOut).
- Cross-platform: `CreateProcessW` on Windows (preserves the existing implementation), `fork() + execvp` on POSIX. Pipe redirection + polling loop. Defaults: 120 s timeout, 4 MB cap (override per-spec — harness gets 30 min / 64 MB).
- `stdoutLineSink` enables live-streaming: when set, each newline-terminated chunk is delivered as it arrives (line buffered) for `.progress.log`-style tailing.
- Pure helpers (argv quoting, env-block construction) live in `SubprocessCapturePure.{h,cpp}` per AGENTS.md § "Pure-helper TU-split recipe" (the main TU includes `<windows.h>` which is a banned dep for the doctest rig).
- After lift: `P4Blame.cpp` calls `SubprocessCapture::Run(...)` with p4-specific defaults; behaviour unchanged. Verified by re-running existing p4-related smokes.

**`CodingHarnessTypes`** — `Source_Core/include/CodingHarnessTypes.h` (new).
- `SeedPacket` — `proposalId`, `sourceIssueKey`, `issueTitle`, `issueBody`, `issueComments` (vector), `proposedAction` (enum mirror of `AgentProposal::proposedAction`), `proposalRationale`, `repoRoot`, `branchName`, `worktreePath`, `agentsTreePath`. Pure data.
- `HarnessRunStatus` — enum: `Pending | Spawning | Running | AwaitingUser | PrOpen | Iterating | Complete | Failed | Cancelled`.
- `HarnessRunEvent` — `timestamp`, `phase` (matches `agent-progress.sh` phase enum), `message`, optional `payload` JSON.

**`ICodingHarnessRunner`** — `Source_Core/include/ICodingHarnessRunner.h` (new interface).
- `virtual void Start(const SeedPacket&, HarnessCallbacks)` — spawns, returns immediately. `HarnessCallbacks` carries `onProgressLine`, `onStateChange`, `onClarificationRequest`, `onPrOpened`, `onComplete`, `onFailed` — all invoked on the worker thread; consumers post to UI via `MainThreadDispatcher`.
- `virtual void SubmitClarification(const std::string& proposalId, const std::string& response)` — writes `USER_RESPONSE.json`, signals resume.
- `virtual void Resume(const std::string& proposalId, const std::string& additionalContext)` — re-spawn with `--resume` + extra context (used by `PrCommentWatcher` for iterations).
- `virtual void Cancel(const std::string& proposalId)`.
- `virtual std::string Name() const` — for logging / runner-selection.

**`ClaudeCodeLocalRunner`** — `Source_Core/{include,src}/ClaudeCodeLocalRunner.{h,cpp}` (new; first concrete impl).
- Resolves `claude` binary via `$SMATCHET_HARNESS_BIN` env var, falls back to `PATH` lookup, fails with a clear error if missing.
- Creates worktree: `git worktree add .claude/worktrees/agent-<proposalId> -b agent/<proposalId>/<short-slug> origin/develop`. Base is `origin/develop`, not `develop`, because the caller's main worktree typically has `develop` checked out and `git worktree add` refuses to re-check-out an already-active branch. If `origin/develop` is stale, the runner runs `git fetch origin develop` first (gated behind a config flag `handoff.auto_fetch_before_worktree`, default `true`). Worktree root + branch name flow into `SeedPacket`.
- Writes `SEED.md` (human-readable system+context for harness) + `SEED.json` (structured data — proposal payload, issue body, comments) into the worktree.
- Spawns: `claude --print --output-format stream-json --permission-mode bypassPermissions --prompt-file SEED.md` (verify exact flag names at phase 3 against the SDK's current CLI surface; the SDK is under active development).
- Captures stdout via `SubprocessCapture` with `stdoutLineSink` — each stream-json event is parsed, surfaces as a `HarnessRunEvent`, and also fanned out to a `tee` of `.progress.log` (so `bash scripts/dev/tail-agent.sh agent-<id>` works unchanged).
- Polls the worktree for sentinel files (`CLARIFICATION_NEEDED.json`, `RUN_RESULT.json`, `ERROR.json`, `PR_URL.txt`) every 500 ms during `Running` (cheap; worktree on local disk).
- After harness signals success and writes `PR_URL.txt`, runs `git push -u origin agent/<proposalId>/<short-slug>` and `gh pr create --draft --title <…> --body <SEED.md slice + proposal rationale>` (the harness itself can do this — preferred so it's part of the harness's tool-call audit trail; runner only does it as a fallback if `PR_URL.txt` isn't written within N seconds of completion).
- Worktree GC: on `Complete | Failed | Cancelled`, the worktree is **kept** by default (so the user can inspect). Explicit cleanup via `handoff.gc --older-than-days N` command.

**`PrCommentWatcher`** — `Source_Core/{include,src}/PrCommentWatcher.{h,cpp}` (new).
- For every `HarnessRunState` in `PrOpen`, the watcher polls `GET /repos/{o}/{r}/pulls/{n}/comments` + `GET /repos/{o}/{r}/issues/{n}/comments` every `pr_poll_interval_sec` (default 120 s) via `GitHubClient` (companion plan's `GitHubClient`, must exist).
- Maintains a per-PR cursor (`last_seen_comment_id`) in `agent_pr_watch` SQLite table.
- On a new non-bot comment, dispatches `runner.Resume(proposalId, formattedComments + pull-request-diff)` and transitions state to `Iterating`.
- Bot-comment filter: skip comments authored by the same identity that opened the PR (avoid feedback loop on the harness's own progress comments).
- Watcher loop runs on a dedicated `std::thread` owned by `AgenticHandoffController`; cancel-atomic + shutdown-safe.

**`AgenticHandoffController`** — `Source_Core/{include,src}/AgenticHandoffController.{h,cpp}` (new).
- Owned by `AppController` as `std::unique_ptr<AgenticHandoffController>`, macro-gated `#if SMATCHET_WITH_AI` (Standalone only — DX12 omits the whole TU).
- API:
  - `StartHandoff(const std::string& proposalId)` — fetches proposal, builds `SeedPacket`, picks runner (config-driven; phase 1 always `ClaudeCodeLocalRunner`), calls `runner.Start(...)`.
  - `SubmitClarification(proposalId, response)` — forwards to runner.
  - `CancelHandoff(proposalId)`.
  - `MarkPrMerged(proposalId)` / `MarkPrClosed(proposalId)` — terminal transitions, used by the watcher or by manual user action.
  - `ListInFlight()` — returns snapshot of `HarnessRunState` for the UI.
- Subscribes to `AgentProposalStore::OnProposalApproved` (new callback signal on the store, added in this plan) so an approval auto-triggers a handoff when `agentic.handoff.auto_start_on_approve` is true. When false, the user clicks `Start handoff` in the proposals panel.
- Cross-source create proposals (`DerivedTicketCreate` from the companion plan): the **companion plan's** `AgenticTriageController` owns the actual `ITrackerClient::CreateIssue` call when the user approves a `DerivedTicketCreate` proposal (that's part of proposal-apply, not handoff). After the derived ticket exists, the proposal's `payload.derivedKey` is populated. Only then does `AgenticHandoffController` (this plan's controller) consume the now-resolved proposal and seed the harness using the **derived** tracker key as `sourceIssueKey`. Single seed packet per proposal; no chained handoffs.

**`SmatchetAgentHandoffUi`** — `Source_Core/src/SmatchetAgentHandoffUi.{h,cpp}` (new) + draw hook in `SmatchetUI.cpp`.
- Modeless dockable panel (pattern: `SmatchetAiAssistantUi`).
- Top half: table of in-flight runs — `proposal-id | source-issue | status | started | phase`. Click row → expanded detail below.
- Detail panel: `.progress.log` tail (read-only `InputTextMultiline`), current state, [Cancel] button, [Open PR] (if `PR_URL.txt`) → `ShellExecuteW` per `BlameAnalysisUi_Launch.cpp:1-50`, [Open worktree] → `ShellExecuteW` on the worktree path.
- When status is `AwaitingUser`: an `InputTextMultiline` box appears with the harness's question + a `Submit` button → calls `controller.SubmitClarification(...)`.
- Keyboard nav: `Tab` cycles rows, `Enter` opens detail, `Ctrl+Enter` submits clarification (per pillar 4).
- WCAG AA contrast via `SmatchetTheme` (audit at phase 7).

**Commands** — registered in `Source_Core/src/Commands/Builtin/BuiltinCommands_Handoff.cpp` (new TU; pattern follows `BuiltinCommands_Automation.cpp`).
- `handoff.start <proposal-id>` — explicit kick-off (also wired as the auto-action from proposal approval when `auto_start_on_approve` is true).
- `handoff.list [--status pending|running|...]`.
- `handoff.show <proposal-id>` — dumps `HarnessRunState` JSON.
- `handoff.clarify <proposal-id> <response>` — non-UI clarification path (for CLI / MCP / Lua callers).
- `handoff.cancel <proposal-id>`.
- `handoff.gc --older-than-days <n>` — prune completed-task worktrees + DB rows.
- `handoff.dry-run <proposal-id>` — assembles seed packet, writes to worktree, **does not spawn the harness**. For debugging the seed shape.
- All write paths marked `Destructive = true` → existing `ErrorCode::ConfirmRequired` flow on CLI/MCP/Lua.

**Scenario step** — `Source_Core/src/Commands/Scenarios/AgentHandoffScenarioStep.cpp` (new).
- Reuses `IScenario`. Single step `handoff.run` invokes the controller with a stub `ICodingHarnessRunner` that returns a deterministic `RUN_RESULT.json` from a fixture. Purely for replay regression — no real subprocess in scenario mode.

**Config additions** — `Source_Core/include/ConfigManager.h` + `.cpp`:
```json
{
  "handoff": {
    "enabled": false,
    "runner": "claude-code-local",
    "harness_bin": "",
    "harness_timeout_ms": 1800000,
    "worktree_root": ".claude/worktrees",
    "auto_start_on_approve": false,
    "pr_poll_interval_sec": 120,
    "pr_iteration_budget": 10,
    "branch_prefix": "agent/",
    "draft_pr": true
  }
}
```
Schema-version bump (for the new `agent_pr_watch` SQLite table + `agent_proposals.handoff_status` column) **held until phase 10** per AGENTS.md § "Schema-version bumps".

## Phased rollout

Each phase = one PR = one `cmake --build` + one `scripts/dev/test-all.sh` at the slice boundary. Plan-lock claim per AGENTS.md § "Parallel-plan pre-flight" — claim slug `agentic-coding-handoff` before phase 1. **This plan assumes `agentic-triage-flow` phases 0–5 have shipped** (the controller consumes `AgentProposal` rows from the store the companion plan builds). Phases 5–6 of this plan depend on `agentic-triage-flow` phase 2 (`GitHubClient` write methods).

| Phase | Scope | Key files | Gate |
|---|---|---|---|
| 0 | Plan-lock claim + ADR + glossary | `docs/design/agentic-coding-handoff.md`, `docs/adr/00NN-pluggable-coding-harness-runner.md`, `docs/CONTEXT.md` | doc-only |
| 1 | `SubprocessCapture` lift from `P4Blame`. Pure helpers (argv quoting / env build). Doctest with `echo` / `true` / `false` / `sleep` mocks. Repoint `P4Blame.cpp` to use the new helper. Both targets compile. | `Source_Core/{include,src}/SubprocessCapture.{h,cpp}`, `Source_Core/{include,src}/SubprocessCapturePure.{h,cpp}`, `Source_Core/src/P4Blame.cpp` (rewired), `tests/Source_Core/SubprocessCapturePure.test.cpp`, `tests/Source_Core/SubprocessCapture.test.cpp` | dual-target build + ctest + manual P4 smoke (blame view still works) |
| 2 | `CodingHarnessTypes` + `ICodingHarnessRunner` + `CodingHarnessSeedBuilder` (pure: assemble SEED.md + SEED.json from `AgentProposal`). Doctest seed builder. **Plus** the new `agents/handoff-implementer.md` agent file + the new `AGENTS.md § Handoff envelope` section (matched to the SEED.json shape this phase defines). | `Source_Core/include/CodingHarnessTypes.h`, `Source_Core/include/ICodingHarnessRunner.h`, `Source_Core/{include,src}/CodingHarnessSeedBuilder.{h,cpp}`, `tests/Source_Core/CodingHarnessSeedBuilder.test.cpp`, `agents/handoff-implementer.md`, `AGENTS.md` | build + ctest + manual frontmatter review (schema mirrors `agents/code-review.md`, version=1) |
| 3 | `ClaudeCodeLocalRunner` — spawn via `SubprocessCapture`, write SEED files, parse stream-json stdout, poll sentinel files, surface events via callbacks. Worktree creation via `git worktree add` (use `SubprocessCapture`). Stub `claude` binary in tests (a bash script that echoes canned stream-json + writes sentinel files). | `Source_Core/{include,src}/ClaudeCodeLocalRunner.{h,cpp}`, `tests/fixtures/stub-claude/stub-claude.sh`, `tests/Source_Core/ClaudeCodeLocalRunner.test.cpp` | build + ctest + CLI smoke (`Smatchet.exe cmd handoff.dry-run …`) |
| 4 | `HarnessRunState` FSM + `AgenticHandoffController`. Manual `handoff.start <id>` command. Wires through one full lifecycle with the stub runner (Pending → Spawning → Running → Complete). Audit trail entries on every transition. | `Source_Core/include/HarnessRunState.h`, `Source_Core/{include,src}/AgenticHandoffController.{h,cpp}`, `Source_Core/src/Commands/Builtin/BuiltinCommands_Handoff.cpp`, `Source_Core/src/Commands/BuiltinCommands.cpp`, `Source_Core/src/AppController.cpp`, `tests/Source_Core/HarnessRunState.test.cpp`, `tests/Source_Core/AgenticHandoffController.test.cpp` | build + ctest + CLI smoke with stub runner |
| 5 | Clarification dual-channel. `CLARIFICATION_NEEDED.json` / `USER_RESPONSE.json` round-trip in the worktree + parallel GitHub-issue-comment post via the companion plan's `GitHubClient::AddIssueCommentPlain`. Reply-poll loop matches comments back to in-flight handoffs. Resume the harness via `--resume`. | `Source_Core/src/ClaudeCodeLocalRunner.cpp` (clarification path), `Source_Core/src/AgenticHandoffController.cpp` (poll glue), `tests/Source_Core/ClaudeCodeLocalRunner.test.cpp` (extend), CLI smoke `test-agentic-handoff-clarification.sh` | build + ctest + CLI smoke |
| 6 | PR-open path. `gh pr create --draft` via `SubprocessCapture` (fallback if harness doesn't write `PR_URL.txt`). Validates returned URL, transitions to `PrOpen`. Toast on success. | `Source_Core/src/ClaudeCodeLocalRunner.cpp` (push + pr-create fallback), `Source_Core/include/ConfigManager.h` (handoff block), `tests/Source_Core/ClaudeCodeLocalRunner.test.cpp` (extend with `gh` stub) | build + ctest + CLI smoke |
| 7 | `PrCommentWatcher` — poll-loop thread, dedupe via SQLite cursor, dispatch `runner.Resume(...)`. Budget enforcement (`pr_iteration_budget` — abort after N iterations). **Plus** the new `agents/pr-iterator.md` agent file (lands alongside the watcher it pairs with). | `Source_Core/{include,src}/PrCommentWatcher.{h,cpp}`, `Source_Core/src/AgenticHandoffController.cpp` (own the watcher), `tests/Source_Core/PrCommentWatcher.test.cpp` (with recorded comment fixtures), `agents/pr-iterator.md`, CLI smoke `test-agentic-handoff-iterate.sh` | build + ctest + CLI smoke + manual frontmatter review |
| 8 | `SmatchetAgentHandoffUi` panel. In-flight table, expanded detail, clarification input box, PR-URL `ShellExecuteW` open, worktree open. Keyboard nav. Toast wiring on state changes. | `Source_Core/src/SmatchetAgentHandoffUi.{h,cpp}`, `Source_Core/src/SmatchetUI.cpp` (draw hook), `Source_Core/src/SmatchetUI_MainMenu.cpp` (menu entry), `Source_Core/src/SmatchetPreferencesUi.cpp` (handoff config row) | build + ctest + screenshot diff |
| 9 | Cross-flow wiring: when companion plan's `SmatchetAgentProposalsUi` calls `agent.proposal.approve <id>`, the proposal store's `OnProposalApproved` callback (added here) fires and (config-gated) auto-starts a handoff. Adds a `[Start handoff]` button on the proposals row for the manual path. | `Source_Core/src/AgentProposalStore.cpp` (callback signal), `Source_Core/src/SmatchetAgentProposalsUi.cpp` (button), `Source_Core/src/AgenticHandoffController.cpp` (subscribe) | build + ctest + manual click-through |
| 10 | Scenario step + recorded fixtures for replay regression. Schema-version bump (new `agent_pr_watch` table + `agent_proposals.handoff_status` column). Plan-revision sections appended (`## Implementation log`, `## Deviations from plan`, `## Verification`). | `Source_Core/src/Commands/Scenarios/AgentHandoffScenarioStep.cpp`, `tests/fixtures/handoff_seed_sample.json`, `tests/fixtures/pr_comments_sample.json`, `Source_Core/include/ConfigManager.h` (schema bump), `docs/design/agentic-coding-handoff.md` (revisions) | full regression: `scripts/dev/test-all.sh` + dual-target build + screenshot diff + sanitizer build |

All phases use the full build + test-all loop. Not eligible for the trivial-visual-only envelope (C++ surface, threading, subprocess, IPC).

## Critical files (read references, not modified unless noted)

- `Source_Core/src/P4Blame.cpp` — source for the `SubprocessCapture` lift (phase 1 modifies it to call the new helper).
- `Source_Core/include/AppController.h` / `.cpp` — owns the controller; modified phase 4.
- `Source_Core/include/MainThreadDispatcher.h` — reuse `PostToMainThread` for worker→UI callbacks. Read-only.
- `Source_Core/include/SmatchetToast.h` — reuse `SmatchetToastManager::Push` for state-change notifications. Read-only.
- `Source_Core/src/SmatchetAiAssistantUi.cpp` — reference template for the modeless dockable panel pattern.
- `Source_Core/src/SmatchetOfflineQueueUi.cpp:1069` — reference for the `InputTextMultiline` + scratch-buffer pattern (used for clarification reply box).
- `Source_Core/src/BlameAnalysisUi_Launch.cpp:1-50` — reference for `ShellExecuteW` URL launch (PR + worktree open).
- `Source_Core/include/TicketSyncService.h:67-84` — reference for the FSM-with-worker pattern (`StreamingSyncState` shape).
- `Source_Core/include/BackendAuditTrail.h` — reuse `AppendEvent` for every state transition; redaction handles the seed payload.
- `Source_Core/include/AgentProposalStore.h` (from companion plan) — extended in phase 9 with the `OnProposalApproved` callback.
- `Source_Core/include/GitHubClient.h` (from companion plan) — used in phase 5 (issue comment), phase 6 (PR open if harness doesn't), phase 7 (PR-comment poll). Hard dependency.
- `scripts/setup-harness.sh` — read-only. The script already wires `.claude/agents` for whichever harness Smatchet's own author runs; phase 3 verifies that the spawned `claude` child sees the same agent tree (it inherits `cwd` = the worktree, which is inside the repo, so the same `.claude/` is resolved). The two new agent files (`handoff-implementer`, `pr-iterator`) land in `agents/` and are exposed automatically — no setup-harness.sh changes.
- `AGENTS.md` — modified phase 2 (new § Handoff envelope section under § Delegation). Read-only otherwise.
- `agents/handoff-implementer.md`, `agents/pr-iterator.md` — created phase 2 and phase 7 respectively. Frontmatter schema mirrors `agents/code-review.md` / `agents/debug-detective.md`.
- `scripts/dev/agent-progress.sh` / `tail-agent.sh` — read-only. The harness child emits `[HH:MM:SS] <phase>: <message>` lines to `.progress.log` (either via the SDK's progress-event surface or via explicit shell-outs to `agent-progress.sh` from Bash tools); Smatchet just tails the file.

## Verification (deterministic, zero manual residue target)

Per AGENTS.md § "Verification automation — zero manual steps". `test-author` invoked at plan-time (here), post-first-round (after phase 4), and after every handoff that involves a manual step.

- **Pure-logic doctest**:
  - `SubprocessCapturePure.test.cpp` — argv quoting (Windows MSC-style + POSIX-style), env-block construction, timeout-monotonic-clock math.
  - `CodingHarnessSeedBuilder.test.cpp` — SEED.md formatting deterministic across two `AgentProposal` fixtures, SEED.json schema round-trip.
  - `HarnessRunState.test.cpp` — every state transition (valid + invalid).
  - `PrCommentWatcher.test.cpp` — cursor advancement against fixed comment fixtures, bot-filter, budget enforcement.
- **Subprocess doctest** (real `SubprocessCapture` against tiny child processes):
  - `SubprocessCapture.test.cpp` — `echo hello` exit-0 capture, `false` exit-1, `sleep 5 + 100 ms timeout` → `timedOut = true`, `printf "%s" "$(seq 1 100000)" + cap = 1 KB` → truncated, `kill` signal handling.
- **Stub-runner end-to-end** (Bash script masquerading as `claude`):
  - `tests/fixtures/stub-claude/stub-claude.sh` — emits canned stream-json events + writes `RUN_RESULT.json`. Used by `ClaudeCodeLocalRunner.test.cpp` to exercise the full state machine without touching the real CLI.
- **CLI smoke** (`scripts/dev/test-agentic-handoff-*.sh`, auto-enrolled):
  - `test-agentic-handoff-cli.sh` — `Smatchet.exe cmd handoff.dry-run <fixture-id>`; asserts SEED files appear in worktree.
  - `test-agentic-handoff-clarification.sh` — drives the clarification round-trip via the stub runner.
  - `test-agentic-handoff-iterate.sh` — PR-comment-watch loop hits the iteration cap.
- **Screenshot diff** — phase 8 panel rendered against three fixture in-flight runs (Running / AwaitingUser / PrOpen).
- **ImGui Test Engine** (not wired today): clicking [Submit clarification], [Cancel], [Open PR]. Flagged to `docs/backlog/agent-self-improvement/tooling.md` with concrete action plan.
- **Sanitizer build** — ASan/UBSan via `ninja-test-msys2`. Required because the new code adds subprocess plumbing + multiple worker threads + SQLite mutations.
- **Dual-target compile** — `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`. `SubprocessCapture` builds in both (no AI dep). Controller / runner / watcher / UI gate behind `#if SMATCHET_WITH_AI`; verify DX12 still compiles cleanly.

**End-to-end happy-path probe** (phase 10, after automation passes):
1. Configure `handoff.runner = claude-code-local`, point `harness_bin` to a real `claude` install.
2. From the companion plan's `SmatchetAgentProposalsUi`, approve a real proposal targeting a real test repo.
3. Either auto-start fires (config) or user clicks `[Start handoff]`.
4. Watch the in-flight row transition: Spawning → Running. Watch `.progress.log` tail in the panel.
5. (If the harness asks for clarification): question appears in panel + as GitHub-issue comment. Answer in the panel.
6. Wait for PR open. Click PR link, verify it opens in browser, branch is `agent/<proposalId>/<slug>`, PR is `--draft`.
7. Leave a review comment on the PR; watch `PrCommentWatcher` re-spawn the harness; verify a new commit lands within poll-interval + run time.
8. Merge the PR; verify state transitions to `Complete` and the worktree row marks as complete (worktree kept on disk for inspection).

Manual residue from steps 4-8 → `test-author` handoff to wire ImGui-Test-Engine equivalents.

## Out of scope (deferred)

- Parallel in-flight runs. Phase 1: at most one handoff at a time per source-issue. Multiple proposals queue; the controller serialises. Multi-runner concurrency deferred — needs deeper thought on worktree-locking and PR-spam avoidance.
- Cloud / remote harness runner. `ClaudeCodeCloudRunner` is a phase-N follow-up; the interface accommodates it.
- Codex / Aider runners. Same interface; phase-N follow-ups.
- Pre-merge CI gating from inside the harness. The harness opens a draft PR; CI must pass before the user merges. No "auto-merge on green" in this plan.
- Custom commit / PR author identity. Phase 1 commits as the user's local `git config user.*`; `gh pr create` uses the user's `gh auth status` identity. Bot identities deferred.
- Worktree GC automation. Phase 1 keeps worktrees on disk; explicit `handoff.gc --older-than-days N` only. Auto-GC after merge deferred.
- Multi-repo handoffs. Phase 1 only handles handoffs whose `sourceIssueKey` resolves to a repo whose worktree is the current Smatchet repo. Cross-repo deferred.

## Risks + mitigations

- **Spawned harness goes rogue inside the worktree.** Mitigation: worktree is on a branch (never `develop`), every PR is `--draft`, every action audited, `pr_iteration_budget` caps revision loops, user can cancel from the panel at any time.
- **Claude Code CLI flag surface drifts.** Mitigation: phase 3 captures the exact flag set against the SDK's current docs (`claude --help`); a startup-time `claude --version` probe gates the feature; mismatch logs a clear error and disables the runner. SDK upgrades trigger a manual phase-3 re-test.
- **Worktree explosion fills disk.** Mitigation: worktrees per task default to `.claude/worktrees/agent-<id>`; `handoff.gc` available; UI panel shows on-disk size per worktree; a backlog entry tracks auto-GC.
- **PR poll loop becomes a feedback spam channel.** Mitigation: bot-comment filter on `PrCommentWatcher` (skip same identity that opened PR + the harness's own progress comments), `pr_iteration_budget` hard cap, exponential backoff on empty polls.
- **Clarification race (panel + GitHub answer simultaneously).** Mitigation: `USER_RESPONSE.json` write is the canonical signal; the GitHub-comment poll path also writes the same file; both paths take an idempotency lock on the proposalId. First write wins; second is logged but a no-op.
- **Plan-lock collision.** This plan's write-set heads: `Source_Core/src/P4Blame.cpp`, `Source_Core/src/AppController.cpp`, `Source_Core/include/AppController.h`, `Source_Core/include/ConfigManager.h`, `Source_Core/src/ConfigManager.cpp`, `Source_Core/src/Commands/BuiltinCommands.cpp`, `Source_Core/src/SmatchetUI.cpp`, `Source_Core/src/SmatchetUI_MainMenu.cpp`, `Source_Core/src/SmatchetPreferencesUi.cpp`, `Source_Core/src/SmatchetAgentProposalsUi.cpp` (companion-plan file), `Source_Core/src/AgentProposalStore.cpp` (companion-plan file, phase-9 callback signal), `AGENTS.md`, `agents/handoff-implementer.md`, `agents/pr-iterator.md`, `tests/CMakeLists.txt`. Run `bash scripts/dev/locks-show.sh` before phase 1; phase 9 collides intentionally with the companion plan's UI + store files — coordinate with the companion-plan implementer. Note: `AGENTS.md` is a high-traffic file across the agent ecosystem; check for any in-flight prompt-engineering / self-improvement merges before claiming.
- **Pillar 1/2 regression.** Every spawn + subprocess wait + GitHub poll is on a worker thread; UI thread does `MainThreadDispatcher::Drain` + the panel render only. Verify with `perf-detective` on the standard scenario before phase 10 merge.
- **Dual-target breakage.** `#if SMATCHET_WITH_AI` gates the new controller / runner / watcher / UI / commands. `SubprocessCapture` itself has zero AI dep — it builds in both targets and replaces `P4Blame`'s embedded copy.
- **Harness CLI not installed on the user's box.** Mitigation: startup-time probe (`claude --version` via `SubprocessCapture`) — if missing, disable the handoff feature gracefully, surface a one-shot toast (`Handoff disabled: 'claude' not found on PATH`), and surface a "How to install" link in the preferences pane.
- **Stream-json schema drift.** Mitigation: the parser accepts unknown event types (logs at `LOG_TRACE`, doesn't fail). Schema validation enforces only the fields the controller depends on (`type`, `subtype`, `result`).
- **GitHub rate-limits hit during PR-comment poll.** Mitigation: poll interval defaults to 120 s, backs off to 600 s on 429, fails open (no spam re-tries).
- **`gh` not authenticated.** Mitigation: same probe as `claude` — `gh auth status` at startup; if not OK, feature disabled with a clear preferences-pane message.
- **`.claude/agents/` junction missing.** The spawned harness only sees the agent tree if `.claude/agents` exists as a junction (or symlink) into `agents/`. On a fresh clone where `bash scripts/setup-harness.sh claude-code` has not been run, the junction is absent and the spawned harness operates without agent context. Mitigation: `ClaudeCodeLocalRunner::Probe` checks for the junction at startup and runs `bash scripts/setup-harness.sh claude-code` automatically (or surfaces a "run setup-harness.sh first" toast + disables the runner if the script fails / is missing).
- **Nested-harness case.** Smatchet may itself be running inside a Claude Code session (the user develops Smatchet using Claude Code). Spawning a child `claude` from a parent `claude` is supported by the CLI (each invocation is a fresh session), but the child's `cwd` (the new worktree) does **not** inherit the parent's open files / context — that's expected and correct, the child is independent. Worth verifying once during phase 3 by spawning from inside a parent Claude Code session and asserting the child produces a PR end-to-end.

## Reused functions / utilities (do not re-implement)

| Need | Use |
|---|---|
| Subprocess spawn + stdout/stderr capture | `SubprocessCapture::Run` (phase 1 lift from `P4Blame::RunProcessCapture`) |
| Worker thread + cancel atomic | `AppController::LaunchBackgroundTask` (`AppController.h:979`, `AppController.cpp:544-563`) |
| Worker → UI hand-off | `MainThreadDispatcher::PostToMainThread` (`MainThreadDispatcher.h`) |
| Transient notification | `SmatchetToastManager::Push` (`SmatchetToast.h`) |
| Modeless dockable panel | `SmatchetAiAssistantUi` skeleton |
| Multiline text input | `SmatchetOfflineQueueUi.cpp:1069` pattern |
| URL / path open in browser/explorer | `BlameAnalysisUi_Launch.cpp:1-50` (`ShellExecuteW`) |
| FSM-with-worker pattern | `TicketSyncService::StreamingSyncState` (`TicketSyncService.h:67-84`) |
| Audit trail | `BackendAuditTrail::AppendEvent`, `RedactJson` (`BackendAuditTrail.h`) |
| Live-progress channel | `scripts/dev/agent-progress.sh` (emits) + `tail-agent.sh` (consumes) — harness writes naturally, UI panel just tails the file |
| Agent-tree exposure to spawned harness | `bash scripts/setup-harness.sh claude-code` — already wires `.claude/agents` → `agents/` (no per-spawn work needed; the worktree inherits the repo's `.claude/`) |
| GitHub HTTP (PR-create + comment-poll) | `GitHubClient` (companion plan; this plan adds `CreatePullRequest`, `FetchPrComments` if not already present) |
| Proposal source-of-truth | `AgentProposalStore` (companion plan; this plan adds `OnProposalApproved` callback in phase 9) |
| Schema versioning pattern | `LocalCacheManager` migration logic (reference; do not subclass) |
| Command registration | `CommandRegistry::RegisterCommand({...})` pattern across `BuiltinCommands_*.cpp` |
| Scenario step | `IScenario` + `ScenarioRunner::Tick` (`Source_Core/include/Commands/Scenarios/IScenario.h`) |
| Env-var read with default | `EnvOr` / `EnvIntOr` (`Target_Standalone/CliCommandRunner.cpp:332-340`) |

## Open questions (to confirm before phase 1)

1. **Branch naming.** `agent/<proposalId>/<short-slug>` vs `agent/<short-slug>` vs `bot/agent/<short-slug>`. Recommend `agent/<proposalId>/<short-slug>` (proposalId disambiguates same-issue retries; slug aids human reading). Confirm before phase 3.
2. **Worktree root location.** `.claude/worktrees/` is consistent with the rest of the harness infra and gitignored already. Recommend keeping that. Confirm before phase 3.
3. **`claude` CLI exact flag set.** `--print --output-format stream-json --permission-mode bypassPermissions --prompt-file <path>` is the current understanding. Phase 3 verifies against the SDK CLI of the day; if any flag has changed, the runner is the single place to fix.
4. **Bot-comment-filter identity.** Filter by `gh auth status` user, or by a hardcoded comment-prefix the harness emits (e.g. `<!-- smatchet-handoff -->`)? Recommend a comment-prefix marker for resilience against multi-account environments. Confirm at phase 7.
5. **Auto-start on approve.** Default `false` to preserve human-in-loop posture; the user explicitly clicks `[Start handoff]` per approval. Confirm before phase 9.
6. **PR iteration budget default.** `pr_iteration_budget = 10` proposed (i.e. up to 10 re-spawns per PR before forcing user attention). Higher / lower? Confirm before phase 7.
7. **Harness env passthrough.** Should `SMATCHET_*` env vars from the parent Smatchet process flow into the spawned harness (could leak secrets) or get scrubbed (default-deny with an explicit allow-list)? Recommend default-deny + allow-list `["PATH", "HOME", "USER", "USERPROFILE", "TEMP", "GH_TOKEN", "GITHUB_TOKEN", "ANTHROPIC_API_KEY"]`. Confirm before phase 3.
