# Agent self-improvement — bug

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

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
