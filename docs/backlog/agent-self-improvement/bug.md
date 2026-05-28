# Agent self-improvement — bug

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-21 · orchestrator · [bug] · P1 — code-color slices 5+6+7 (#353, `56841193`) shipped with long-text editor + tooltip not visibly applying syntax colors
  Details: PR #353 was force-merged on CR timeout with user-acknowledged in-session breakage: "long-text editor + tooltip don't visibly color" while the annotate window DOES color. User identified the likely cause in-session before the wrap: missing `Colorize(0, -1)` invocation on the long-text editor + tooltip code paths after the language definition is bound. The annotate-window path calls it (works); the other two paths bind the language def but never trigger the initial tokenization pass, so the editor renders without highlight. Visible on develop now. Build is clean; no crash; UI just falls back to default text color in long-text-edit / tooltip code surfaces.
  Concrete next action: in `Source_Core/src/SmatchetLongTextEditorUi.cpp` + the callstack-tooltip render path (likely `Source_Core/src/CallstackTooltipRender.cpp` — verify by grep), find the spot where `editor.SetLanguageDefinition(...)` is called and add `editor.Colorize(0, -1);` immediately after (forces a full re-tokenize on the new lang). Pattern reference: the annotate-window path in `Source_Core/src/BlameAnalysisUi.cpp` (or similar — verify via `grep -n "Colorize" Source_Core/src/`). Add a doctest if a pure-logic seam exists; otherwise a bucket-E ImGui-Test-Engine scenario via `tests/ui/code_color_long_text_editor.test.cpp` (mirror `tests/ui/views_columns_reorder.test.cpp`'s shape). ~1-2 h: fix is small but bucket-E coverage takes most of the time. Filed as P1 because shipping a half-broken visual feature to develop trains users to expect colors that aren't there.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-20 · orchestrator · [bug] · P3 — Three UI-thread sync-I/O sites not yet moved to workers (Pillar 2 follow-up from Slice 2 migration)
  Details: Slice 2 of `docs/design/archive/pillar-1-2-perf-review-system.md` ran `bash scripts/dev/pillar2-scan.sh` against the full first-party tree + migrated the worker-bound false positives by annotating with `/* PILLAR2_WORKER_ONLY */ // est-latency:` markers. Three hits remain that are NOT worker-bound — UI-thread sync reads with bounded sizes today but flagged for migration. Tracked via `// TODO(pillar2): bug-2026-05-20-ui-sync-reads` comments at each site so the scanner reports them as WARN (not CRITICAL — doesn't block the lint gate). Sites: (1) `Source_Core/src/SmatchetAttachmentPreviewUi.cpp:61` `ParseImageDimensions` — reads the entire attachment file into memory on the UI thread to parse the first 24 bytes for PNG/JPEG dimensions. Up to the 50 MB attachment limit. Easy fix: `seekg` + read 64 bytes. (2) `Source_Core/src/SmatchetPlanDocViewerUi.cpp:95` `ReadCapped` — UI-thread read of `docs/design/*.md` / `docs/adr/*.md` on combo-change. 1 MiB cap, local disk, typically sub-ms but legitimately sync on UI. Could move to worker with `MainThreadDispatcher::PostToMainThread` callback. (3) `Plugins/LuaConsole/LuaConsolePlugin.cpp:92` `ReadFileAll` — Lua script load on editor-open (UI thread). Small scripts (typically < 100 KB), sub-ms typical. Could move to worker but the load-on-edit flow is a one-time cost.
  Concrete next action: fix in priority order: (1) ParseImageDimensions — high-impact (50 MB hot path), low effort (~30 min — switch to seekg + 64-byte read). (2) ReadCapped — low-impact (1 MiB cap), low effort (~30 min — worker + dispatcher post-back). (3) ReadFileAll — lowest impact (small files, one-time), defer until a real user reports a hitch. After fix, remove the TODO marker so the scanner stops emitting WARN.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-19 · coderabbit-react-loop · [bug] · P3 — `WhisperAiAssistantAutosendScenario.cpp` references unguarded `UiDrawSession::assistantPanelOpen` — develop DX12 link broken
  Details: Surfaced by the coderabbit-react-loop phase-2 agent (PR #288). The file at `Source_Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp:112/113/309` references `g_ui.assistantPanelOpen` without an `#if defined(SMATCHET_WITH_AI)` guard. The field is NOT renamed/removed — it lives at `SmatchetUiSession.h:187` inside the `#if defined(SMATCHET_WITH_AI)` block. Standalone defines `SMATCHET_WITH_AI=1` via `SmatchetCoreAiShim`; the DX12 target compiles `Source_Core/` TUs with `SMATCHET_WITH_AI` undefined per the contract documented at `CMakeLists.txt:815-820`. Result: `cmake --build --preset ninja-iter-msvc --target SmatchetCore_DX12` fails on develop HEAD.
  Impact: every `Source_Core/`-touching agent that follows AGENTS.md § Project rules' dual-target verify hits a pre-existing break and must triage-by-stash. The standalone build is fine; DX12 targets are `EXCLUDE_FROM_ALL` so CI doesn't catch it, but agents performing the manual verify see false positives. Re-confirmed reproducing on 2026-05-20 during AI chat Claude-Desktop-parity Phase 1.
  Concrete next action: wrap the three call sites in `#if defined(SMATCHET_WITH_AI)` so they no-op when AI is off, OR (cleaner) gate the whole `WhisperAiAssistantAutosendScenario` registration on `SMATCHET_WITH_AI && SMATCHET_WITH_WHISPER` in CMakeLists.txt — autosend has nothing to do without the AI panel. ~30 min once chosen.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-18 · debug-detective · [bug] · P2 — `SmatchetAiAssistantUi.cpp` `#define ImGui SmatchetLocalizedImGui` macro is invisible at call sites
  Details: While investigating the whisper splice-no-show (PR #258), the verbose `[temp-debug] a7b2c4 HookDictation REGISTER` log fired for `s_inputCharBuf` even though the AI Assistant TU appeared to call raw `ImGui::InputTextMultiline` (which doesn't go through the wrapper hook). 2 detective rounds were spent grepping for `SmatchetLocalizedImGui::InputTextMultiline` callers (none) before noticing the TU-local `#define ImGui SmatchetLocalizedImGui` at line 21. The macro rewrites every `ImGui::` call in the TU to the wrapper transparently. Greppable indirection (`using namespace`) would have shaved the investigation by half.
  Concrete next action: replace `#define ImGui SmatchetLocalizedImGui` with explicit `using namespace SmatchetLocalizedImGui;` (the wrapper's `using namespace ::ImGui;` inside the namespace handles the fallthrough to underlying ImGui functions). Audit all TUs that do the same macro trick and apply uniformly. ~30 min for the AI Assistant TU + grep-and-sweep across the codebase.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-18 · debug-detective · [bug] · P3 — `SmatchetLocalizedImGui::HookDictationOnLastItem` lives inline in a hot header
  Details: Same investigation as the macro entry above. Adding `[temp-debug]` instrumentation to the hook required `#include "Logger.h"` + `<unordered_map>` in `Source_Core/include/SmatchetLocalizedImGui.h` — both contagious to every TU that pulls the wrapper. Long compile churn while iterating on the temp-debug spec; non-trivial cleanup risk (one missed include leaks Logger into hot paths).
  Concrete next action: split `HookDictationOnLastItem` out into a thin `Source_Core/src/SmatchetDictationHook.cpp` with the impl out-of-line behind a forward-declared free function in the header (signature unchanged: `void HookDictationOnLastItem(char*, std::size_t)`). Future debug instrumentation lives in the .cpp without touching every includer. ~1 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · code-review · [bug] · P3 — `AiSseParser::Flush()` synthesises `\n\n` boundary so a final non-terminated chunk delivers as a token
  Details: `AiSseParser.cpp:91` appends `"\n\n"` to the in-progress buffer then re-enters `Feed(nullptr, 0, ...)` to force-emit a final frame. If a malicious or buggy server sends a final non-terminated chunk that happens to parse as a valid SSE frame body, it gets dispatched as a token even though the server never indicated the frame was complete. Low-impact (just a delivered chunk) but the policy "discard residual partial frame on Flush" is safer.
  Concrete next action: change `Flush` to clear `buffer_` without re-feeding (drop the partial frame). Update `AiSseParser.test.cpp` "many small Feeds" or add a new test asserting Flush on `"data:partial"` (no boundary) emits zero events. ~20 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/src/AiClientFactory.cpp:14,17,20` uses `new OpenAiClient()` wrapped in `unique_ptr` instead of `std::make_unique`
  Details: Violates AGENTS.md § Quality "no raw `new`/`delete` — use `std::unique_ptr` + `make_unique`".
  Concrete next action: rewrite three call sites to `std::make_unique<OpenAiClient>(...)`. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/include/AiSseParser.h:38` + `.cpp:101` — `partial_` member + `emitIfReady` is a stub no-op
  Details: Either dead code or unfinished — must be resolved before Phase B of the AI assistant work.
  Concrete next action: delete or wire up before Phase B. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/src/TicketSyncService.cpp:86` empty-fetch guard is permanent; legitimately empty cache never reconverges
  Details: A user who legitimately deletes the last ticket or filters all rows never reconverges (stale cache forever). The guard installed via the prior empty-fetch fix is unconditional.
  Concrete next action: timestamp + age-out, or require two consecutive empty full-syncs before allowing the wipe. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `Source_Core/src/PlaneIssueSearch.cpp:480` asymmetric vs `JiraIssueSearch.cpp:393` for empty-page handling
  Details: Jira requires `fetchedPages > 0`; Plane does not. The new TicketSyncService guard is the only thing standing between a zero-page Plane response and a wipe.
  Concrete next action: align Plane's empty-page handling with Jira's `fetchedPages > 0` predicate. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [bug] · P2 — `LuaToJson` / `JsonToLua` + `kJsonToLuaMaxDepth` duplicated verbatim across `AppController_LuaBindingsCore.cpp` ↔ `AppController_LuaBindings.cpp`
  Details: File comment names the duplication as intentional (post-split keeps Core ImGui-free). Drift risk on the next marshalling change.
  Concrete next action: lift to a shared internal header reachable from both TUs. Surfaced by retrospective code-review sweep on PR #144.
  Status: open
  Last-reviewed: 2026-05-17
