# Agent self-improvement — debt

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug (deprecated) · debt · process · tooling · infra · test · security · external-blockers · applied.
> **Product tech-debt** — internal maintainability with NO user-observable defect
> (god-object, duplication, coupling, missing abstraction, "should refactor"). A
> defect a user or a correctness/safety gate would observe is a **GitHub Issue**,
> not a debt entry — see [`../../agent-rules/issue-triage.md`](../../agent-rules/issue-triage.md).
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-28 · deep-audit · [debt] · P2 — `AppController` is a ~1110-line / ~150-method god-object spanning ~9 concerns (code-health)
  Details: `Source/Core/include/AppController.h` is ~1110 lines, ~150 public methods across tracker sync, field-catalog/field-edit + editmeta caching, offline create/field-edit queues, connectivity probing, Lua automation, AI assistant, MCP activity, attachments, app-update, and host callbacks. Owns 10+ `unique_ptr` subsystems + ~10 mutexes; implemented across 10 partial-class `.cpp` files (~7.5k LOC). Highest-coupling node — the natural merge-conflict + reasoning bottleneck. Decomposition is already underway and principled (`OfflineQueueService` / `TicketSyncService` / `LuaAutomationHost` extracted behind ISP `*Deps` interfaces with fakes-for-tests). Supersedes the stale `backlog/BACKLOG_CODE_REVIEW.md` N4 (predates the friend-coupling removal). Verified (deep-audit, adversarially confirmed: header 1110 lines, 10 mutexes, 10 partial TUs). Migrated from `bug.md` (ADR-0014) — code-health, not a user-observable defect.
  Concrete next action: continue the extraction — lift the connectivity-probe FSM and the field-edit/editmeta-cache cluster into their own services behind narrow `*Deps` interfaces; group the optional host-callback setters (OpenUrl / CloseEmbeddedUi / AttachmentViewer / OpenFilePaths / RequestAppQuit) into one `HostCallbacks` struct injected at `Initialize`. Target a thin facade delegating to owned services. Multi-PR.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-28 · deep-audit · [debt] · P3 — Three minor architecture/coupling cleanups (code-health)
  Details: (1) Extracted services still depend on `AppController`'s nested result structs: `Source/Core/include/Sync/OfflineQueueService.h:32` `#include`s the full `AppController.h` and returns `AppController::DeadLetterRestoreSummary` / `*DeleteSummary` (:65-93) — behaviour is inverted via `IOfflineQueueDeps` but the data contract still lives on the god-object, so the service header can't compile standalone. (2) `Source/Core/src/PluginHost.cpp:124` constructs `McpPlugin` by name (`make_unique<McpPlugin>(port)`), the one core spot `IPlugin` isn't fully inverted — build-gated + ADR-0002-governed + dual-target-safe, but Source/Core isn't strictly plugin-agnostic. (3) `Source/Core/src/Tracker/DefaultTrackerBackendFactory.cpp:19-25` hides a synchronous `ConfigManager::Load()` disk-read inside `Create("github")` while the Jira/Plane branches construct arg-less — asymmetric, bakes credentials into the instance. All verified (deep-audit, adversarially confirmed).
  Concrete next action: (1) relocate the queue/dead-letter result structs to `Source/Core/include/Sync/OfflineQueueTypes.h` + re-export aliases on AppController for back-compat; (2) register a `PluginHost::SetMcpPluginFactory(...)` callback wired from bootstrap (mirror `ITrackerBackendFactory`); (3) pass a `TrackerConfig` snapshot into `Create(type, const TrackerConfig&)` or have `GitHubClient` read config lazily like Jira/Plane. Each ~1 h, independent.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-18 · debug-detective · [debt] · P3 — `SmatchetLocalizedImGui::HookDictationOnLastItem` lives inline in a hot header
  Details: Adding `[temp-debug]` instrumentation to the hook required `#include "Logger.h"` + `<unordered_map>` in `Source/Core/include/SmatchetLocalizedImGui.h` — both contagious to every TU that pulls the wrapper. Long compile churn while iterating on the temp-debug spec; non-trivial cleanup risk (one missed include leaks Logger into hot paths). Migrated from `bug.md` (ADR-0014) — a header-hygiene / compile-time concern, no user-observable defect.
  Concrete next action: split `HookDictationOnLastItem` out into a thin `Source/Core/src/SmatchetDictationHook.cpp` with the impl out-of-line behind a forward-declared free function in the header (signature unchanged: `void HookDictationOnLastItem(char*, std::size_t)`). Future debug instrumentation lives in the .cpp without touching every includer. ~1 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [debt] · P2 — `LuaToJson` / `JsonToLua` + `kJsonToLuaMaxDepth` duplicated verbatim across `AppController_LuaBindingsCore.cpp` ↔ `AppController_LuaBindings.cpp`
  Details: File comment names the duplication as intentional (post-split keeps Core ImGui-free). Drift risk on the next marshalling change. Migrated from `bug.md` (ADR-0014) — duplication, no user-observable defect.
  Concrete next action: lift to a shared internal header reachable from both TUs. Surfaced by retrospective code-review sweep on PR #144.
  Status: open
  Last-reviewed: 2026-05-17
