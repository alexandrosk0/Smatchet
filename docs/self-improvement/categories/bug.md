# Agent self-improvement — bug

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-28 · deep-audit · [bug] · P2 — 11 empty `catch(...){}` blocks lack the mandated `// catch-all-ok:` marker
  Details: `docs/agent-rules/exception-handling-policy.md` hard-rule #1 makes an unmarked empty `catch(...){}` a review CRITICAL. 11 such blocks are on develop: `Source/Standalone/CliCommandRunner.cpp:809,955,1074,1245,1390`; `Source/Core/src/TicketGridModel.cpp:46`; `Source/Core/src/Sync/OfflineQueueService.cpp:476,698`; `Source/Core/src/TicketFieldEditorLongTextPure.cpp:26`; `Source/Core/src/Tracker/PlaneIssueMutation.cpp:328`; `Source/Standalone/StandaloneAppBootstrap.cpp:294`. Running `.claude/hooks/lint-catch-all.py` on CliCommandRunner.cpp returns rc=2 with the exact five `[error]` lines. Most bodies are defensible (e.g. CliCommandRunner.cpp:807-810 is a `std::stoi` parse-fallback) but the policy requires the marker regardless. They shipped because the lint is a local PostToolUse hook, not a CI gate (paired tooling entry). Verified exhaustively (deep-audit, multiline-aware scan — exactly these 11).
  Concrete next action: add `// catch-all-ok: <reason>` to each of the 11 blocks (or a `LOG_DEBUG` where a silent swallow masks signal). ~45 min. Pair with the tooling entry that CI-gates `lint-catch-all.py`.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-28 · deep-audit · [bug] · P2 — `AppController` is a ~1110-line / ~150-method god-object spanning ~9 concerns (code-health)
  Details: `Source/Core/include/AppController.h` is ~1110 lines, ~150 public methods across tracker sync, field-catalog/field-edit + editmeta caching, offline create/field-edit queues, connectivity probing, Lua automation, AI assistant, MCP activity, attachments, app-update, and host callbacks. Owns 10+ `unique_ptr` subsystems + ~10 mutexes; implemented across 10 partial-class `.cpp` files (~7.5k LOC). Highest-coupling node — the natural merge-conflict + reasoning bottleneck. Decomposition is already underway and principled (`OfflineQueueService` / `TicketSyncService` / `LuaAutomationHost` extracted behind ISP `*Deps` interfaces with fakes-for-tests). Supersedes the stale `backlog/BACKLOG_CODE_REVIEW.md` N4 (predates the friend-coupling removal). Verified (deep-audit, adversarially confirmed: header 1110 lines, 10 mutexes, 10 partial TUs).
  Concrete next action: continue the extraction — lift the connectivity-probe FSM and the field-edit/editmeta-cache cluster into their own services behind narrow `*Deps` interfaces; group the optional host-callback setters (OpenUrl / CloseEmbeddedUi / AttachmentViewer / OpenFilePaths / RequestAppQuit) into one `HostCallbacks` struct injected at `Initialize`. Target a thin facade delegating to owned services. Multi-PR.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-28 · deep-audit · [bug] · P3 — Three minor architecture/coupling cleanups (code-health)
  Details: (1) Extracted services still depend on `AppController`'s nested result structs: `Source/Core/include/Sync/OfflineQueueService.h:32` `#include`s the full `AppController.h` and returns `AppController::DeadLetterRestoreSummary` / `*DeleteSummary` (:65-93) — behaviour is inverted via `IOfflineQueueDeps` but the data contract still lives on the god-object, so the service header can't compile standalone. (2) `Source/Core/src/PluginHost.cpp:124` constructs `McpPlugin` by name (`make_unique<McpPlugin>(port)`), the one core spot `IPlugin` isn't fully inverted — build-gated + ADR-0002-governed + dual-target-safe, but Source/Core isn't strictly plugin-agnostic. (3) `Source/Core/src/Tracker/DefaultTrackerBackendFactory.cpp:19-25` hides a synchronous `ConfigManager::Load()` disk-read inside `Create("github")` while the Jira/Plane branches construct arg-less — asymmetric, bakes credentials into the instance. All verified (deep-audit, adversarially confirmed).
  Concrete next action: (1) relocate the queue/dead-letter result structs to `Source/Core/include/Sync/OfflineQueueTypes.h` + re-export aliases on AppController for back-compat; (2) register a `PluginHost::SetMcpPluginFactory(...)` callback wired from bootstrap (mirror `ITrackerBackendFactory`); (3) pass a `TrackerConfig` snapshot into `Create(type, const TrackerConfig&)` or have `GitHubClient` read config lazily like Jira/Plane. Each ~1 h, independent.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-21 · orchestrator · [bug] · P1 — code-color slices 5+6+7 (#353, `56841193`) shipped with long-text editor + tooltip not visibly applying syntax colors
  Details: PR #353 was force-merged on CR timeout with user-acknowledged in-session breakage: "long-text editor + tooltip don't visibly color" while the annotate window DOES color. User identified the likely cause in-session before the wrap: missing `Colorize(0, -1)` invocation on the long-text editor + tooltip code paths after the language definition is bound. The annotate-window path calls it (works); the other two paths bind the language def but never trigger the initial tokenization pass, so the editor renders without highlight. Visible on develop now. Build is clean; no crash; UI just falls back to default text color in long-text-edit / tooltip code surfaces.
  Concrete next action: in `Source/Core/src/SmatchetLongTextEditorUi.cpp` + the callstack-tooltip render path (likely `Source/Core/src/CallstackTooltipRender.cpp` — verify by grep), find the spot where `editor.SetLanguageDefinition(...)` is called and add `editor.Colorize(0, -1);` immediately after (forces a full re-tokenize on the new lang). Pattern reference: the annotate-window path in `Source/Core/src/BlameAnalysisUi.cpp` (or similar — verify via `grep -n "Colorize" Source/Core/src/`). Add a doctest if a pure-logic seam exists; otherwise a bucket-E ImGui-Test-Engine scenario via `tests/ui/code_color_long_text_editor.test.cpp` (mirror `tests/ui/views_columns_reorder.test.cpp`'s shape). ~1-2 h: fix is small but bucket-E coverage takes most of the time. Filed as P1 because shipping a half-broken visual feature to develop trains users to expect colors that aren't there.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-20 · orchestrator · [bug] · P3 — Three UI-thread sync-I/O sites not yet moved to workers (Pillar 2 follow-up from Slice 2 migration)
  Details: Slice 2 of `docs/plans/shipped/pillar-1-2-perf-review-system.md` ran `bash scripts/dev/pillar2-scan.sh` against the full first-party tree + migrated the worker-bound false positives by annotating with `/* PILLAR2_WORKER_ONLY */ // est-latency:` markers. Three hits remain that are NOT worker-bound — UI-thread sync reads with bounded sizes today but flagged for migration. Tracked via `// TODO(pillar2): bug-2026-05-20-ui-sync-reads` comments at each site so the scanner reports them as WARN (not CRITICAL — doesn't block the lint gate). Sites: (1) `Source/Core/src/SmatchetAttachmentPreviewUi.cpp:61` `ParseImageDimensions` — reads the entire attachment file into memory on the UI thread to parse the first 24 bytes for PNG/JPEG dimensions. Up to the 50 MB attachment limit. Easy fix: `seekg` + read 64 bytes. (2) `Source/Core/src/SmatchetPlanDocViewerUi.cpp:95` `ReadCapped` — UI-thread read of `docs/plans/active/*.md` / `docs/adr/*.md` on combo-change. 1 MiB cap, local disk, typically sub-ms but legitimately sync on UI. Could move to worker with `MainThreadDispatcher::PostToMainThread` callback. (3) `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:92` `ReadFileAll` — Lua script load on editor-open (UI thread). Small scripts (typically < 100 KB), sub-ms typical. Could move to worker but the load-on-edit flow is a one-time cost.
  Concrete next action: fix in priority order: (1) ParseImageDimensions — high-impact (50 MB hot path), low effort (~30 min — switch to seekg + 64-byte read). (2) ReadCapped — low-impact (1 MiB cap), low effort (~30 min — worker + dispatcher post-back). (3) ReadFileAll — lowest impact (small files, one-time), defer until a real user reports a hitch. After fix, remove the TODO marker so the scanner stops emitting WARN.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-19 · coderabbit-react-loop · [bug] · P3 — `WhisperAiAssistantAutosendScenario.cpp` references unguarded `UiDrawSession::assistantPanelOpen` — develop DX12 link broken
  Details: Surfaced by the coderabbit-react-loop phase-2 agent (PR #288). The file at `Source/Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp:112/113/309` references `g_ui.assistantPanelOpen` without an `#if defined(SMATCHET_WITH_AI)` guard. The field is NOT renamed/removed — it lives at `SmatchetUiSession.h:187` inside the `#if defined(SMATCHET_WITH_AI)` block. Standalone defines `SMATCHET_WITH_AI=1` via `SmatchetCoreAiShim`; the DX12 target compiles `Source/Core/` TUs with `SMATCHET_WITH_AI` undefined per the contract documented at `CMakeLists.txt:815-820`. Result: `cmake --build --preset ninja-iter-msvc --target SmatchetCore_DX12` fails on develop HEAD.
  Impact: every `Source/Core/`-touching agent that follows AGENTS.md § Project rules' dual-target verify hits a pre-existing break and must triage-by-stash. The standalone build is fine; DX12 targets are `EXCLUDE_FROM_ALL` so CI doesn't catch it, but agents performing the manual verify see false positives. Re-confirmed reproducing on 2026-05-20 during AI chat Claude-Desktop-parity Phase 1.
  Concrete next action: wrap the three call sites in `#if defined(SMATCHET_WITH_AI)` so they no-op when AI is off, OR (cleaner) gate the whole `WhisperAiAssistantAutosendScenario` registration on `SMATCHET_WITH_AI && SMATCHET_WITH_WHISPER` in CMakeLists.txt — autosend has nothing to do without the AI panel. ~30 min once chosen.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-18 · debug-detective · [bug] · P2 — `SmatchetAiAssistantUi.cpp` `#define ImGui SmatchetLocalizedImGui` macro is invisible at call sites
  Details: While investigating the whisper splice-no-show (PR #258), the verbose `[temp-debug] a7b2c4 HookDictation REGISTER` log fired for `s_inputCharBuf` even though the AI Assistant TU appeared to call raw `ImGui::InputTextMultiline` (which doesn't go through the wrapper hook). 2 detective rounds were spent grepping for `SmatchetLocalizedImGui::InputTextMultiline` callers (none) before noticing the TU-local `#define ImGui SmatchetLocalizedImGui` at line 21. The macro rewrites every `ImGui::` call in the TU to the wrapper transparently. Greppable indirection (`using namespace`) would have shaved the investigation by half.
  Concrete next action: replace `#define ImGui SmatchetLocalizedImGui` with explicit `using namespace SmatchetLocalizedImGui;` (the wrapper's `using namespace ::ImGui;` inside the namespace handles the fallthrough to underlying ImGui functions). Audit all TUs that do the same macro trick and apply uniformly. ~30 min for the AI Assistant TU + grep-and-sweep across the codebase.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-18 · debug-detective · [bug] · P3 — `SmatchetLocalizedImGui::HookDictationOnLastItem` lives inline in a hot header
  Details: Same investigation as the macro entry above. Adding `[temp-debug]` instrumentation to the hook required `#include "Logger.h"` + `<unordered_map>` in `Source/Core/include/SmatchetLocalizedImGui.h` — both contagious to every TU that pulls the wrapper. Long compile churn while iterating on the temp-debug spec; non-trivial cleanup risk (one missed include leaks Logger into hot paths).
  Concrete next action: split `HookDictationOnLastItem` out into a thin `Source/Core/src/SmatchetDictationHook.cpp` with the impl out-of-line behind a forward-declared free function in the header (signature unchanged: `void HookDictationOnLastItem(char*, std::size_t)`). Future debug instrumentation lives in the .cpp without touching every includer. ~1 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [bug] · P3 — `AiSseParser::Flush()` synthesises `\n\n` boundary so a final non-terminated chunk delivers as a token
  Details: `AiSseParser.cpp:91` appends `"\n\n"` to the in-progress buffer then re-enters `Feed(nullptr, 0, ...)` to force-emit a final frame. If a malicious or buggy server sends a final non-terminated chunk that happens to parse as a valid SSE frame body, it gets dispatched as a token even though the server never indicated the frame was complete. Low-impact (just a delivered chunk) but the policy "discard residual partial frame on Flush" is safer.
  Concrete next action: change `Flush` to clear `buffer_` without re-feeding (drop the partial frame). Update `AiSseParser.test.cpp` "many small Feeds" or add a new test asserting Flush on `"data:partial"` (no boundary) emits zero events. ~20 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source/Core/src/AiClientFactory.cpp:14,17,20` uses `new OpenAiClient()` wrapped in `unique_ptr` instead of `std::make_unique`
  Details: Violates AGENTS.md § Quality "no raw `new`/`delete` — use `std::unique_ptr` + `make_unique`".
  Concrete next action: rewrite three call sites to `std::make_unique<OpenAiClient>(...)`. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source/Core/include/AiSseParser.h:38` + `.cpp:101` — `partial_` member + `emitIfReady` is a stub no-op
  Details: Either dead code or unfinished — must be resolved before Phase B of the AI assistant work.
  Concrete next action: delete or wire up before Phase B. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source/Core/src/TicketSyncService.cpp:86` empty-fetch guard is permanent; legitimately empty cache never reconverges
  Details: A user who legitimately deletes the last ticket or filters all rows never reconverges (stale cache forever). The guard installed via the prior empty-fetch fix is unconditional.
  Concrete next action: timestamp + age-out, or require two consecutive empty full-syncs before allowing the wipe. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source/Core/src/PlaneIssueSearch.cpp:480` asymmetric vs `JiraIssueSearch.cpp:393` for empty-page handling
  Details: Jira requires `fetchedPages > 0`; Plane does not. The new TicketSyncService guard is the only thing standing between a zero-page Plane response and a wipe.
  Concrete next action: align Plane's empty-page handling with Jira's `fetchedPages > 0` predicate. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `LuaToJson` / `JsonToLua` + `kJsonToLuaMaxDepth` duplicated verbatim across `AppController_LuaBindingsCore.cpp` ↔ `AppController_LuaBindings.cpp`
  Details: File comment names the duplication as intentional (post-split keeps Core ImGui-free). Drift risk on the next marshalling change.
  Concrete next action: lift to a shared internal header reachable from both TUs. Surfaced by retrospective code-review sweep on PR #144.
  Status: open
  Last-reviewed: 2026-05-17
