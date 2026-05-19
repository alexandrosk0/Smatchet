# Agentic Issue Triage Flow — Plan

> **Slug:** `agentic-triage-flow`. Plan-mode scratch lived at `~/.claude/plans/add-agentinc-flow-for-abundant-flurry.md`; this is the canonical home per AGENTS.md § "Plan location".

## Context

The user wants a human-in-the-loop agentic flow that can:

1. Pick up issues from an external source (GitHub Issues first, then Jira/Plane already supported).
2. Run a triage step on each new/updated issue using a **local LLM** (e.g. Ollama at `localhost:11434`).
3. Produce concrete **proposals** (label, assign, comment, transition state, or create a derived ticket in Jira/Plane) that the user must explicitly approve before any tracker write happens.
4. Persist proposals + decisions in an audit trail so the loop is durable across restarts.

The motivating split: the **GitHub-specific** part is a new `ITrackerClient` backend; the **agnostic core** (everything above `ITrackerClient`) is the agentic loop, the proposal store, the local-LLM caller, and the approval UI. New tracker sources later (e.g. Linear, Azure DevOps) drop in by implementing `ITrackerClient`.

**Why now:** Smatchet already has the foundation — `AiAssistantController`, `OllamaClient` (NDJSON streaming), `AgentsMdLoader`, `BackendAuditTrail`, `MainThreadDispatcher`, the unified `CommandRegistry`, and the `ITrackerClient` abstraction. The agentic flow is mostly **wiring** on top of existing parts, not new infrastructure. User constraint: **low complexity** for the agent entry points (suitable for a small local model — 7B–8B class).

## Decisions locked (from user clarifying questions)

| Question | Decision |
|---|---|
| Source-agnostic abstraction | **`ITrackerClient` is the abstraction.** GitHub gets a full `GitHubClient` implementing it; methods that don't map (JQL, sprints, worklog) return documented "unsupported" values. Agent core only ever talks to `ITrackerClient`. |
| LLM transport | **New thin `AgenticInferenceClient`** — one-shot blocking POST → JSON parse, no chat lifecycle. Sits on top of `cpr` (and optionally borrows the lower-level transport bits of `OllamaClient`). Does **not** reuse `AiAssistantController` (which is shaped for interactive chat). |
| Triggers | **Manual command + scheduled poll + scenario step.** No GitHub webhook receiver in this plan. |
| Action scope | **Full write surface, all human-gated.** Comment / label / assign / transition (close, reopen) / cross-source ticket creation (derive a Jira or Plane ticket from a GitHub issue). Zero writes without approval. |

## Architecture

```
+--------------------------------------------------------+
|  AgenticTriageController        (agnostic core)        |
|  - source: ITrackerClient&  (GitHub | Jira | Plane)    |
|  - target: ITrackerClient*  (for cross-source create)  |
|  - inference: AgenticInferenceClient                   |
|  - proposalStore: AgentProposalStore                   |
|  - cancelToken: std::atomic<bool>                      |
+--------------------------------------------------------+
       |                |                  |
       v                v                  v
   ITrackerClient   AgenticInfer..    AgentProposalStore
       |             Client             |
       |              |                 v
   +---+---+          v            SQLite table
   |Jira/  |     cpr::Post         + audit trail
   |Plane/ |     -> Ollama         + JSONL append
   |GitHub*|     /api/chat
   +-------+
```

`*` = new. Everything else exists.

### Per-component sketch

**`GitHubClient`** — `Source_Core/{include,src}/GitHubClient.{h,cpp}` (new).
- Implements `ITrackerClient`. Auth: `Authorization: Bearer <PAT>` (extend `BuildTrackerHeaders` with a bearer variant, or inline header construction inside `GitHubClient`).
- Issue key format: `<owner>/<repo>#<number>` for cross-repo work; falls back to `#<number>` when scoped to a single repo.
- Wraps the GitHub REST v3 / GraphQL v4 endpoints. Reuse `TrackerHttpRequestWithRetry`, `TrackerHttpResult` classification, and `BackendAuditTrail` per the existing `JiraClient` / `PlaneClient` template.
- Method-by-method behaviour:
  - `FetchIssues(query)` → `GET /search/issues?q=<query>` (GitHub search syntax) or `GET /repos/{o}/{r}/issues` (label filter).
  - `FetchIssueComments()` (new virtual on `ITrackerClient` — see "Interface additions" below) → `GET /repos/{o}/{r}/issues/{n}/comments`. Required by the companion `agentic-coding-handoff` plan for `SEED.json` (issue body + thread). Adding it to the interface (with a default `outError = "FetchIssueComments is not supported"` body) preserves the locked abstraction that agent core only ever talks through `ITrackerClient`.
  - `FetchFieldCatalog()` → returns a minimal catalog: labels, assignees, milestones, state (open/closed). No custom fields.
  - `UpdateIssueFields()` → maps to `PATCH /repos/{o}/{r}/issues/{n}` (labels, assignees, milestone, state, title, body).
  - `CreateIssue()` → `POST /repos/{o}/{r}/issues`.
  - `AddIssueCommentPlain()` → `POST /repos/{o}/{r}/issues/{n}/comments`.
  - `FetchIssueWatchers()`, `FetchIssueVotes()` → map to GitHub reactions / subscribers when feasible.
  - `SearchUsersByQuery()` → `GET /search/users`.
  - `BuildBrowseUrl()` → `https://github.com/{o}/{r}/issues/{n}`.
  - `AddIssueToSprint()`, `AddWorklog()`, `AttachFilesToIssue()` (binary multipart), `ExtractProjectFromQuery()` → return per each method's signature a documented "unsupported" value: empty vector / default-constructed result struct / `false` for `bool` returns. Each unsupported branch logs `LOG_WARN("GitHub: <method> not supported")` exactly once (latching flag) and returns immediately without an HTTP call. Confirm each method's actual return shape against `Source_Core/include/ITrackerClient.h` during phase 1 — that read is part of the phase-1 work, not pre-decided here.

**Interface additions to `ITrackerClient`** (phase 1):
- New virtual `FetchIssueComments(const TrackerConfig& cfg, const std::string& issueKey, std::vector<TrackerIssueComment>& outComments, std::string& outError)` with a default body that sets `outError = "FetchIssueComments is not supported by this backend."` and returns `false`. `JiraClient` and `PlaneClient` inherit the default unsupported until a follow-up adds real impls (out of scope here). `GitHubClient` overrides to return the real comment thread.
- New struct `TrackerIssueComment` in `Source_Core/include/ITrackerClient.h` adjacent to `TrackerIssueFetchSummary` — fields: `id`, `author`, `bodyPlain`, `createdAtIso`. Pure data; consumed by the companion plan's `CodingHarnessSeedBuilder`.

**`AgenticInferenceClient`** — `Source_Core/{include,src}/AgenticInferenceClient.{h,cpp}` (new).
- Single public method: `InferenceResult Run(const InferenceRequest&)` (blocking, called from a worker thread). `InferenceRequest` carries: model name, system prompt, user prompt, response-schema hint, timeout. `InferenceResult` carries: `nlohmann::json parsed` + `std::string rawText` + status enum.
- Posts to the configured Ollama endpoint (`/api/chat`, `stream: false`). No NDJSON consumption — agent steps don't need streaming. Borrows `cpr` and reuses the cancel-atomic + timeout patterns from `OllamaClient.cpp:113-193`.
- Parses the response, validates against a small **structured-output schema** (`AgentProposal`-shaped JSON), and returns the parsed proposals.
- Redacts API key and any PII from error logs using `AiErrorRedact::RedactProviderErrorBody`.

**`AgentProposal`** — `Source_Core/include/AgentProposal.h` (new).
- POD struct: `id` (string, unique — generated as `"prop-" + std::to_string(epochMs) + "-" + std::to_string(monotonicCounter)`; no UUID dep added), `sourceTrackerType`, `sourceIssueKey`, `proposedAction` (enum: `CommentAdd | LabelAdd | LabelRemove | AssigneeSet | StateTransition | DerivedTicketCreate | ImplementIssue`), `payload` (`nlohmann::json`), `rationale` (text from LLM), `status` (`Pending | Approved | Rejected | Applied | Failed`), `createdAt`, `decidedAt`, `decidedBy`.
- **Handoff-eligibility filter** — `ImplementIssue` is the **only** action that consents to a coding-harness spawn. The companion plan (`agentic-coding-handoff`) subscribes to `OnProposalApproved` and filters on `proposedAction == ImplementIssue` before invoking `AgenticHandoffController::StartHandoff`. Triage-only actions (label / comment / assign / state / DerivedTicketCreate) apply via the triage controller's own `ITrackerClient` writes and **never** trigger a coding spawn.

**`AgentProposalStore`** — `Source_Core/{include,src}/AgentProposalStore.{h,cpp}` (new).
- SQLite-backed (reuse `SQLiteCpp` already linked via `LocalCacheManager`). One new table `agent_proposals` with the columns above. Migrations via the same versioning hook `LocalCacheManager` uses (`cache_meta` row). **Schema-version bump held until end-to-end verified** per AGENTS.md § "Schema-version bumps".
- API: `Append(proposal)`, `ListPending()`, `MarkApproved(id, by)`, `MarkRejected(id, by)`, `MarkApplied(id)`, `MarkFailed(id, err)`. All decision-change methods also append a `BackendAuditTrail::AppendEvent` row with `type = "agent_proposal_<state>"`.

**`AgenticTriageController`** — `Source_Core/{include,src}/AgenticTriageController.{h,cpp}` (new).
- Owns the agentic loop. One method per trigger:
  - `RunOnce(const TriageRunSpec&)` — fetches recent issues from the configured source, runs each through `AgenticInferenceClient`, appends proposals to the store. Returns a summary (n issues seen / n proposals created / n errors).
  - `StartPoll(intervalSec)` / `StopPoll()` — launches a `std::thread` that calls `RunOnce` every `intervalSec`, cancellable via atomic.
- Lives on `AppController` as a `std::unique_ptr<AgenticTriageController>` member. The whole controller TU and its `AppController` member declaration are macro-gated `#if SMATCHET_WITH_AI` (Standalone-only); DX12 sees neither the header nor the member. Lazy init on first `agent.triage.*` command is a startup-cost optimisation only, not a build-separation mechanism.
- Cross-source create: when a proposal's action is `DerivedTicketCreate`, the proposal payload names the target tracker (Jira/Plane); on apply, the controller resolves that backend through `ITrackerBackendFactory` and calls `CreateIssue` on it.

**`SmatchetAgentProposalsUi`** — `Source_Core/src/SmatchetAgentProposalsUi.cpp` (new) + draw hook in `SmatchetUI.cpp`.
- Modeless ImGui panel (dockable). Table of pending proposals: source key, proposed action, rationale, [Approve] [Reject] [Edit] buttons.
- Keyboard nav: `Tab` cycles row, `Enter` approves, `Esc` rejects, per pillar 4.
- All approval clicks dispatch the matching `agent.proposal.approve <id>` / `agent.proposal.reject <id>` command via `CommandRegistry` → controller does the actual tracker write on a worker thread, posts result back via `MainThreadDispatcher`.
- WCAG AA contrast: reuse `SmatchetTheme` colours; verify in pre-merge.

**Commands (unified registry)** — registered in `Source_Core/src/Commands/Builtin/BuiltinCommands_Agentic.cpp` (new TU; pattern follows the existing `BuiltinCommands_Automation.cpp`):
- `agent.triage.run --source <type> --query <q>` — manual one-round trigger.
- `agent.triage.poll.start --interval-sec <n>` / `agent.triage.poll.stop`.
- `agent.proposal.list [--status pending|approved|...]`.
- `agent.proposal.approve <id>` / `agent.proposal.reject <id>` / `agent.proposal.edit <id> <payload-json>`.
- `agent.proposal.show <id>`.
- All commands marked `Destructive = true` on the write paths so the existing `ErrorCode::ConfirmRequired` flow applies for CLI / MCP / Lua callers.

**Scenario step** — `Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp` (new).
- Reuses the existing `IScenario` API (`OnStart` / `OnFrame` / `IsDone` / `OnFinish`). Single step `agent.triage` invokes `AgenticTriageController::RunOnce` with a recorded HTTP fixture for the source backend and a deterministic `AgenticInferenceClient` stub for the LLM. Purely for **regression testing** — no human-in-loop in scenario mode (auto-approve or auto-reject is a scenario param).

**Config additions** — `Source_Core/include/ConfigManager.h` + `.cpp`:
```json
{
  "github": { "pat": "", "base_url": "https://api.github.com", "default_repos": ["owner/repo"], "search_query": "is:open is:issue label:triage" },
  "agentic": {
    "enabled": false,
    "source_tracker": "github",
    "target_tracker": "jira",
    "poll_interval_sec": 600,
    "inference": {
      "provider": "ollama",
      "base_url": "http://localhost:11434",
      "model": "llama3.1:8b",
      "timeout_ms": 60000,
      "api_key": ""
    },
    "auto_approve_safe_actions": false
  }
}
```
Schema-version bump held until phase 9 (end-to-end verified).

## Phased rollout

Each phase is one PR (one slice = one `cmake --build` + one `test-all` per AGENTS.md § "Build / ctest cadence"). Plan-lock claim per AGENTS.md § "Parallel-plan pre-flight" — claim slug `agentic-triage-flow` before phase 1.

| Phase | Scope | Key files | Gate |
|---|---|---|---|
| 0 | Plan-lock claim + ADR + glossary entries | `docs/design/agentic-triage-flow.md`, `docs/adr/00NN-github-as-itrackerclient.md`, `docs/CONTEXT.md` | doc-only, no build |
| 1 | `ITrackerClient` interface additions: `FetchIssueComments` virtual + `TrackerIssueComment` struct, default unsupported. `GitHubClient` skeleton (read-only methods: `FetchIssues`, `FetchIssueComments`, `BuildBrowseUrl`, `GetTrackerType`). Factory registration. Doctest for pure helpers. | `Source_Core/include/ITrackerClient.h` (additive virtual), `Source_Core/{include,src}/GitHubClient.{h,cpp}`, `Source_Core/{include,src}/GitHubClientHelpers.{h,cpp}` (pure-helper TU per AGENTS.md "Pure-helper TU-split recipe"), `Source_Core/src/DefaultTrackerBackendFactory.cpp`, `tests/Source_Core/GitHubClientHelpers.test.cpp` | dual-target build + ctest |
| 2 | `GitHubClient` write methods (comment / label / assign / state). Audit-trail wiring. CLI smoke. | `Source_Core/src/GitHubClient.cpp`, `Source_Core/include/ConfigManager.h` (GitHub block), `Source_Core/src/Commands/Builtin/BuiltinCommands_*.cpp` (touch only if a smoke command lands here) | dual-target build + ctest + manual CLI smoke against real GitHub repo (deferred to bucket-D screenshot of CLI run by test-author) |
| 3 | `AgenticInferenceClient` (POST + parse + schema validation). Pure-parse helpers in their own TU. Doctest. | `Source_Core/{include,src}/AgenticInferenceClient.{h,cpp}`, `Source_Core/{include,src}/AgenticInferenceClientPure.{h,cpp}`, `tests/Source_Core/AgenticInferenceClientPure.test.cpp` | build + ctest |
| 4 | `AgentProposal` + `AgentProposalStore` (SQLite schema + audit-trail wiring). Doctest for store CRUD + redaction. | `Source_Core/include/AgentProposal.h`, `Source_Core/{include,src}/AgentProposalStore.{h,cpp}`, `tests/Source_Core/AgentProposalStore.test.cpp` | build + ctest |
| 5 | `AgenticTriageController` (loop). Manual `agent.triage.run` command. Worker thread + `MainThreadDispatcher` plumbing. | `Source_Core/{include,src}/AgenticTriageController.{h,cpp}`, `Source_Core/src/Commands/Builtin/BuiltinCommands_Agentic.cpp`, `Source_Core/src/Commands/BuiltinCommands.cpp` (registration call), `Source_Core/src/AppController.cpp` (own the controller) | build + ctest + CLI smoke (`Smatchet.exe cmd agent.triage.run --source github --query "..."`) |
| 6 | `SmatchetAgentProposalsUi` panel + approve/reject/edit commands + audit-trail UI surface. Keyboard nav + WCAG AA verified. Bucket-E test for the Approve/Reject buttons (`tests/ui/agent_proposals_panel.test.cpp` + `scripts/dev/test-ui-agent-proposals.sh`). | `Source_Core/src/SmatchetAgentProposalsUi.{h,cpp}`, `Source_Core/src/SmatchetUI.cpp` (draw hook), `Source_Core/src/SmatchetUI_MainMenu.cpp` (menu entry), `tests/ui/agent_proposals_panel.test.cpp`, `tests/ui/ui_tests_registry.cpp` (registration), `scripts/dev/test-ui-agent-proposals.sh` | build + ctest + bucket-D screenshot diff + bucket-E (`ninja-ui-test-msys2`) |
| 7 | Scheduled poll. `agent.triage.poll.start` / `.stop`. Timer thread. Config wiring + UI toggle. | `Source_Core/src/AgenticTriageController.cpp` (poll loop), `Source_Core/include/ConfigManager.h` (agentic block), `Source_Core/src/SmatchetPreferencesUi.cpp` (toggle row) | build + ctest + CLI smoke |
| 8 | Scenario step. Recorded HTTP fixture for replay regression. | `Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp`, `tests/fixtures/github_issues_sample.json`, `tests/fixtures/ollama_chat_sample.json` | build + ctest + scenario replay |
| 9 | Schema-version bump (SQLite migration). End-to-end verification matrix. Plan revision (`## Implementation log` + `## Deviations from plan` + `## Verification`). | `Source_Core/src/AgentProposalStore.cpp` (CREATE TABLE + `cache_meta` `schema_version` row following the `LocalCacheManager.cpp` pattern at `LocalCacheManager.cpp:60-115`), `docs/design/agentic-triage-flow.md` (revision sections). Note: the `agentic.*` / `github.*` JSON blocks added to `ConfigManager.h` in phases 2 + 7 are **config blocks**, not the SQLite schema bump — they ship per-phase as additive fields, no version gate. | full regression: `scripts/dev/test-all.sh` + dual-target build + bucket-D + bucket-E (`ninja-ui-test-msys2`) |

Phases 1+ all use the **full build + test-all + bucket-E** loop, **not** the trivial-visual-only envelope (this is C++ surface, not theme/locale).

## Critical files (reused, not modified, unless noted)

- `Source_Core/include/ITrackerClient.h` — the agnostic interface; **read-only** for this plan.
- `Source_Core/include/TrackerHttpClient.h` / `TrackerHttpUtils.h` — reuse `TrackerHttpRequestWithRetry`, `TrackerHttpResult` classification. May need a minor extension: a bearer-token variant of `BuildTrackerHeaders` (or inline construction in `GitHubClient` — preferable to avoid touching shared header).
- `Source_Core/include/OllamaClient.h` / `Source_Core/src/OllamaClient.cpp` — **reference only**. `AgenticInferenceClient` does not extend `OllamaClient`; it borrows the `cpr::Post` + cancel-atomic + timeout idiom.
- `Source_Core/include/AiErrorRedact.h` — reuse for inference error redaction.
- `Source_Core/include/AgentsMdLoader.h` — reuse for system-prompt context (the agent should know the project's invariants when triaging).
- `Source_Core/include/BackendAuditTrail.h` — reuse `AppendEvent` for every approval / rejection / apply / fail.
- `Source_Core/include/MainThreadDispatcher.h` — reuse `PostToMainThread` for worker→UI handoffs.
- `Source_Core/include/LocalCacheManager.h` — reference for the SQLite schema-bump pattern that `AgentProposalStore` will follow.
- `Source_Core/include/Commands/Command.h` / `CommandRegistry.h` — register new commands following the existing `RegisterCommand({...})` pattern.
- `Source_Core/include/Commands/Scenarios/IScenario.h` — implement for phase 8 scenario step.
- `Source_Core/src/DefaultTrackerBackendFactory.cpp` — add `"GitHub"` case (factory pattern is already in place per `Create(trackerType: string)`).

## Verification (deterministic, zero manual residue target)

Per AGENTS.md § "Verification automation — zero manual steps", `test-author` is invoked at plan-time + post-first-round + after every agent handoff. Test-author confirms the canonical bucket-letter map (A-E) at plan-time; the categories below describe coverage by capability, mapped to whichever bucket letter test-author returns:

- **Pure-logic doctest** (`SMATCHET_BUILD_TESTS=ON`, `ninja-test-msys2`):
  - `tests/Source_Core/GitHubClientHelpers.test.cpp` — URL builder, query-string encoder, issue-key parser (`owner/repo#N` round-trip), label-add/remove payload builder.
  - `tests/Source_Core/AgenticInferenceClientPure.test.cpp` — schema validator, JSON parse robustness, redaction.
  - `tests/Source_Core/AgentProposalStore.test.cpp` — CRUD on an in-memory SQLite (`:memory:`), audit-trail-line assertion.
- **CLI scenarios** (`scripts/dev/test-agentic-*.sh`, auto-enrolled by `scripts/dev/test-all.sh`):
  - `test-agentic-triage-cli.sh` — `Smatchet.exe cmd agent.triage.run --source github --query <fixture>` against a recorded `github_issues_sample.json` fixture (HTTP intercepted via the same test-mode hook the existing CLI smokes use, if present; if not, gated on a new `--http-fixture` flag added in phase 5).
  - `test-agentic-approve-reject.sh` — runs triage, then approves the first proposal and rejects the second, asserts audit-trail contents.
- **Screenshot diff** (existing screenshot-diff harness):
  - Proposals panel rendered with three fixture proposals — verify layout + contrast.
- **ImGui Test Engine (bucket E, wired)** per `agents/test-author.md` § Bucket E — `ninja-ui-test-msys2` preset, `SMATCHET_BUILD_UI_TESTS=ON`, registration via `IM_REGISTER_TEST` + `SmatchetRegisterAllUiTests`:
  - `tests/ui/agent_proposals_panel.test.cpp` — `Approve` button click on a fixture proposal row asserts the store row transitions to `Approved` + an audit-trail row lands; `Reject` button asserts the symmetric `Rejected` path. Bash driver at `scripts/dev/test-ui-agent-proposals.sh` (auto-enrolled in `test-all.sh`). No manual residue.
- **Sanitizer build** — ASan/UBSan via `ninja-test-msys2`. Required because the new code adds SQLite mutation paths + a worker thread.
- **Dual-target compile** — `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`. `AgenticInferenceClient` and `AgenticTriageController` gate behind `#if SMATCHET_WITH_AI` (Standalone only) — verify DX12 compiles without them.

End-to-end **happy-path manual probe** (phase 9, after all automation passes):

1. Configure `github.pat` + `agentic.source_tracker=github` in `smatchet_config.json`.
2. `Smatchet.exe cmd agent.triage.run --query "is:open is:issue label:bug"` against the real repo.
3. Open the app, observe N pending proposals in the Agent Proposals panel.
4. Approve one labeling proposal; verify the label appears on the GitHub issue and the audit trail records the event with the LLM rationale + the user identity.
5. Reject one comment-create proposal; verify no API call fires, audit trail records the rejection.
6. Start the scheduled poll (`agent.triage.poll.start --interval-sec 300`), wait ~5 min, observe a fresh round of proposals.

Steps 3-6 are bucket-E covered (`scripts/dev/test-ui-agent-proposals.sh` + the CLI smokes above); the happy-path probe is a one-shot sanity check, not a recurring manual gate.

## Out of scope (deferred)

- GitHub webhook receiver (would require exposing an HTTPS endpoint + auth verification). Re-evaluate after phase 9.
- Multi-repo concurrency (poll loop fetches one repo at a time in this plan).
- Streaming LLM responses for agent steps (one-shot only).
- GitHub Enterprise (Server / AE) — config block names `base_url` for future support but only `api.github.com` is tested.
- OAuth flow for GitHub auth — PAT only.
- Per-user proposal filtering (everyone sees all proposals in phase 6 UI; ACL deferred).
- Cross-repo issue linking heuristics (the LLM may suggest "this is a duplicate of #42" but the act of linking is just a comment, not a first-class link).

## Risks + mitigations

- **LLM produces invalid JSON.** Schema validation in `AgenticInferenceClient`; on parse failure, drop the proposal and append a `bad_inference` audit-trail entry rather than crash. Pillar 3.
- **Worker thread races on `AgentProposalStore`.** SQLite serializes writes inside one connection; controller holds the connection and serializes via a mutex. Tested under `:memory:` in doctest.
- **PAT leakage to logs.** All log lines pass through the existing `BackendAuditTrail::RedactJson` + `AiErrorRedact::RedactProviderErrorBody`. Add `pat` / `github_pat` to the redaction key list in phase 2.
- **Schema-version drift across iterations.** Hold the bump until phase 9 per AGENTS.md.
- **Plan-lock collision** with another active slice. Run `bash scripts/dev/locks-show.sh` before phase 1; this plan's write-set heads are `Source_Core/src/DefaultTrackerBackendFactory.cpp`, `Source_Core/src/AppController.cpp`, `Source_Core/include/ConfigManager.h`, `Source_Core/src/ConfigManager.cpp`, `Source_Core/src/Commands/BuiltinCommands.cpp`, `Source_Core/src/SmatchetUI.cpp`, `Source_Core/src/SmatchetUI_MainMenu.cpp`, `Source_Core/src/SmatchetPreferencesUi.cpp`, `tests/CMakeLists.txt`. New files under `Source_Core/{include,src}/` for GitHub/Agentic/Proposal don't collide by definition. If any active claim overlaps any of the head files, coordinate or sequence behind the holding slice.
- **Pillar 1/2 regression.** Every HTTP + SQLite call is on a worker thread; UI thread only does `MainThreadDispatcher::Drain` + the panel render. Verify with `perf-detective` on the standard scenario before phase 9 merge.
- **Dual-target breakage.** `SMATCHET_WITH_AI` gates the new TUs that depend on the LLM caller. `GitHubClient` itself has no AI dependency — it's a plain tracker backend — so it builds in both standalone + DX12.

## Reused functions / utilities (do not re-implement)

| Need | Use |
|---|---|
| HTTP POST with retry + error classification | `TrackerHttpRequestWithRetry` in `Source_Core/include/TrackerHttpClient.h` |
| Auth header (Basic) | `BuildTrackerBasicAuthHeader` in `Source_Core/src/TrackerHttpUtils.cpp` |
| Audit-trail append + redaction | `BackendAuditTrail::AppendEvent`, `RedactJson` in `Source_Core/include/BackendAuditTrail.h` |
| Worker → UI hand-off | `MainThreadDispatcher::PostToMainThread` in `Source_Core/include/MainThreadDispatcher.h` |
| Cancel-atomic + timeout idiom | `OllamaClient::SendStreaming` body, `Source_Core/src/OllamaClient.cpp:113-193` (copy idiom, do not depend on the class) |
| AGENTS.md context for system prompt | `AgentsMdLoader::Load` in `Source_Core/include/AgentsMdLoader.h` |
| Tracker backend factory | `DefaultTrackerBackendFactory::Create` in `Source_Core/src/DefaultTrackerBackendFactory.cpp` |
| SQLite schema versioning pattern | `LocalCacheManager` migration logic (reference; do not subclass) |
| Command registration | `CommandRegistry::RegisterCommand({...})` pattern across `BuiltinCommands_*.cpp` |
| Scenario step shape | `IScenario` + `ScenarioRunner::Tick` in `Source_Core/include/Commands/Scenarios/IScenario.h` |
| Error redaction for LLM errors | `AiErrorRedact::RedactProviderErrorBody` |

## Open questions (to confirm before phase 1)

1. **Issue-key encoding** — `owner/repo#N` vs `owner/repo/N` vs `gh:owner/repo:N`. The existing tracker code treats the issue-key as an opaque string; the prefix matters for parsing in the grid / palette. Recommend `owner/repo#N` (matches GitHub UI convention) — confirm before phase 1.
2. **Bearer-auth header construction** — extend the shared `BuildTrackerHeaders` family (touches a shared header, risk of breaking Jira/Plane) or inline inside `GitHubClient` (more code duplication, lower blast radius). Recommend inline for phase 1; promote to shared helper only if a second bearer-auth backend appears.
3. **Inference response schema** — full structured-output schema document needs writing during phase 3. Suggested shape:
   ```json
   { "proposals": [
       { "action": "LabelAdd", "label": "bug", "rationale": "..." },
       { "action": "AssigneeSet", "user": "alice", "rationale": "..." }
   ] }
   ```
   Confirm field names + enum spelling at phase 3 start.
4. **Poll cursor durability** — store the "last seen issue updated_at" per repo in SQLite or in `smatchet_config.json`? SQLite is more robust against config-file edits; config is simpler. Recommend SQLite in `agent_poll_cursor` table.

## Implementation log

Append-only record per AGENTS.md § Plan revision after implementation. Canonical detail (per-phase summary + per-file write-set) lives in [`docs/design/agentic-flow-implementation.md`](agentic-flow-implementation.md) § Implementation log; this section is a thin index keeping the companion plan self-explanatory.

All 10 triage-half phases shipped (T0–T9):

| Phase | PR  | sha       | Summary                                                                                                |
|-------|-----|-----------|--------------------------------------------------------------------------------------------------------|
| T0    | #225| `906c312` | plan-lock + ADRs 0003/0004 + `docs/CONTEXT.md` seed entries                                            |
| T1    | #226| `c0a74bd` | `ITrackerClient::FetchIssueComments` + `GitHubClient` skeleton + `SMATCHET_WITH_AGENTIC` build gate    |
| T2    | #227| `639b0a3` | `GitHubClient` write methods (`AddComment`/`AddLabel`/`RemoveLabel`/`SetAssignee`/`CloseIssue`) + audit-trail wiring |
| T3    | #228| `0c0f9c6` | `AgenticInferenceClient` (Ollama POST + structured-output schema validation, 6 doctest cases)          |
| T4    | #229| `746d8bf` | `AgentProposal` POD + `AgentProposalStore` (SQLite, 18 doctest cases)                                  |
| T5    | #230| `6530c1f` | `AgenticTriageController::TriageBatch` orchestrator + `agent.triage.run` command                       |
| T6    | #231| `f944dd0` | `SmatchetAgentProposalsUi` panel + bucket-E test (Approve / Reject)                                    |
| T7    | #232| `dd2fa99` | Scheduled-poll worker + Preferences "Agentic" tab + cursor-aware GitHub `since=` filter                |
| T8    | #233| `04a6d0f` | `AgentTriageScenario` step + recorded fixtures + headless replay regression                            |
| T9    | #234| `321589c` | SQLite `schema_version` table + `kCurrentSchemaVersion = 1` + plan revision (triage-half final)        |

## Deviations from plan

Canonical list with per-decision rationale lives in [`agentic-flow-implementation.md`](agentic-flow-implementation.md) § Deviations from plan (T0-T9 block). Headline deviations:

- **Phase-per-branch shipping (not all-in-one).** Plan envisioned one consolidated PR per half; instead each phase shipped as its own PR (T0-T9 = 10 PRs). Rationale: smaller diffs, per-phase regression isolation, parallelisable review.
- **T6 / T7 — issue-title deferred.** Plan-locked decision (#5) allowed deferring issue-title rendering pending T7's `FetchIssueBody` integration; never adopted in this slice. Row header shows `<issueKey>  [<action>]` only. Worth a future UX polish slice if scan-friction surfaces.
- **T7 — cursor advance uses wall-clock, not `max(updated_at)` of returned issues.** GitHub guarantees monotonic `updated_at`; worst-case drift is bounded by the poll interval (≤ 1 h). Parsing every payload would add cost with no observable benefit.
- **T7 — discovery routed through the adapter directly (not `TriageBatch`).** Cursor filter is worker-local; the worker calls `IGitHubReadClient::ListOpenIssuesForRepo` directly + funnels keys through `TriageIssue`. Keeps cursor surface contained.
- **T9 — `schema_version` layered alongside existing additive tables, not via destructive migration.** Decision #14 deferred the bump precisely so T4-T8 could ship without interim version churn. T9 lands `version=1` as the first recorded shipped state; the migration ladder is wired but empty (no version-2 body exists). Singleton row enforced via `CHECK (id = 1)` rather than a separate settings table.

## Verification

The original § Verification block above documents the deterministic target. Shipped verification per phase (build + ctest + dual-target + sanitizer + scenario + bucket-E + CLI smoke) lives in [`agentic-flow-implementation.md`](agentic-flow-implementation.md) § Verification. Triage-half acceptance gate result:

- [x] All T0-T9 PRs merged into `develop`.
- [x] `develop` buildable + `bash scripts/dev/test-all.sh` green at T9 merge.
- [x] `agent.triage.run --source github --query <fixture>` lands proposals in the SQLite store (T5 + T8 scenario verifies; `test-agentic-triage-cli.sh` + `test-agentic-approve-reject.sh` both green).
- [x] `SmatchetAgentProposalsUi` shows proposals with working Approve / Reject buttons (T6 bucket-E variants + T8 scenario verifies state-machine transitions).

Manual residue: none. Triage half is fully stub-verified end-to-end via the scenario step + CLI smoke; no real-PAT / real-LLM probe deferred.
