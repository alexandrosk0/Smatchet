# Plan — Decompose Top-20 Monolith Functions

> **Slug**: `decompose-top-20-monoliths`

## Context

A line-count + branch-count sweep of `Source_Core/src/`, `Plugins/`, `Target_Standalone/` (ThirdParty excluded) surfaced 20 functions over 300 lines each. The top 11 are ImGui draw functions (immediate-mode UI naturally accretes); the remaining 9 are setup, config, payload-build, and HTTP-runner monoliths. Every one of them:

- defeats `clang-tidy` cognitive-complexity warnings,
- forces full-file rereads on every minor edit,
- couples unrelated concerns (data fetch + layout + side-effect dispatch in one body),
- multiplies merge-conflict surface across feature branches,
- inflates per-frame `SMATCHET_UI_PERF_SCOPE` parent-scopes so individual sub-widget costs are invisible to `perf-detective`.

Goal: after this lands, no non-ThirdParty function exceeds **200 lines** and **30 branches**, with a documented ImGui-draw pattern that future authors copy by reflex.

## Approach

### A. ImGui draw-function pattern (canonical)

Authors today write a single `void DrawX(...)` that owns window setup, per-section layout, every state mutation, and every action dispatch. The pattern below extracts at the existing **`SMATCHET_UI_PERF_SCOPE` brace-blocks** — those are already the de-facto section boundaries and they're load-bearing for the perf-review system, so reusing them as the decomposition seam keeps perf scopes identical (zero baseline shift).

Canonical shape per draw function:

```cpp
// Header (.h)
class FooUi {
public:
    void Draw(AppController& app, UiDrawSession& d);

private:
    struct DrawCtx {
        AppController& app;
        UiDrawSession& d;
        // captured-once-per-frame snapshots (e.g. ticketsSnap, columns)
    };

    bool BeginWindow(UiDrawSession& d);    // ImGui::Begin + early-return guard
    void DrawHeader(DrawCtx& ctx);         // toolbar, filters, banner
    void DrawBody(DrawCtx& ctx);           // main table / grid / canvas
    void DrawFooter(DrawCtx& ctx);         // status row, hints, footer
    void DrawModals(DrawCtx& ctx);         // any popups owned by this window
    void HandleHotkeys(DrawCtx& ctx);      // keyboard shortcut dispatch
    // per-section helpers as needed; each ≤ 80 lines
};

// .cpp
void FooUi::Draw(AppController& app, UiDrawSession& d) {
    if (!BeginWindow(d)) { ImGui::End(); return; }
    DrawCtx ctx{ app, d /* + snapshots */ };
    { SMATCHET_UI_PERF_SCOPE("foo:header"); DrawHeader(ctx); }
    { SMATCHET_UI_PERF_SCOPE("foo:body");   DrawBody(ctx);   }
    { SMATCHET_UI_PERF_SCOPE("foo:footer"); DrawFooter(ctx); }
    DrawModals(ctx);
    HandleHotkeys(ctx);
    ImGui::End();
}
```

Rules of the pattern (codified in `docs/agent-rules/imgui-draw-pattern.md`, added by this plan):

1. **`DrawCtx` struct** holds the per-frame snapshots + references. No more 30-line argument lists; no more `static` locals leaking across windows.
2. **One responsibility per helper**. Header / body / footer / modals / hotkeys are non-overlapping. A helper that grows past ~80 lines splits again.
3. **Perf scopes stay at the section boundary**. Helper-internal scopes only if `perf-detective` asks for finer resolution. Scope names match the existing inventory — zero baseline-bump churn.
4. **Window-state extraction**. Persistent `static` locals (filter buffers, expanded-row sets, last-selection ids) move into a `<Foo>WindowState` member struct on the owning UI object. Caught by `grep "static.*Buf\|static bool s_" Source_Core/src/Smatchet*Ui*.cpp`.
5. **Action handlers** (button click → mutation) move into `OnX()` methods returning `void` or `bool`. Keeps the draw body to layout-only.
6. **Section-file split when a `.cpp` exceeds 1500 lines** — precedent: `SmatchetViewsDashboardUi.cpp` + `SmatchetViewsDashboardUi_widgets.cpp`. Naming: `<Owner>Ui_<Section>.cpp`.

### B. Non-UI monolith pattern

For `ConfigManager::Load/Save`, `OnStart`, `BuildUserFieldPayload`, `HtmlToMarkdown`, `RegisterAiCommands`, `RunRequest`, the recipes diverge per call-site:

- **`ConfigManager::Load/Save`** → field-registration table. `static const FieldDesc kFields[]` with `{json_key, getter, setter, default, migration_fn}`; `Load`/`Save` become 20-line loops over the table. Eliminates the parallel duplication risk that currently makes config-key adds error-prone.
- **`McpPlugin::OnStart`** → split into `RegisterCoreRoutes()`, `RegisterToolRoutes()`, `RegisterSseRoutes()`, `StartServer()`. Each ≤ 120 lines.
- **`BuildUserFieldPayload`** → table-driven field-type dispatch (`{FieldType → PayloadBuilder fn}`) + per-type free functions. Cuts the 75-branch tower to a 5-line dispatcher.
- **`HtmlToMarkdown`** → tag-handler table (`{TagName → Renderer fn}`) replacing the if-else chain.
- **`RegisterAiCommands`** → mechanical: each `MakeCommand(...)` block becomes a free `Register<Foo>Command(CommandRegistry&)` function. Pure cut/paste.
- **`AiAssistantController::RunRequest`** → split fetch (HTTP) ↔ stream-parse ↔ history-update phases into separate methods, each testable in isolation.
- **`main` (`Target_Standalone/main.cpp`)** → already partially decomposed; extract remaining `// Boot phase` blocks into `BootApplication()`, `RunFrameLoop()`, `ShutdownApplication()` (`ShutdownApplication` already exists). Goal: `main` ≤ 80 lines.
- **`AppController::Initialize`** → split into `InitConfig()`, `InitBackends()`, `InitCommands()`, `InitPlugins()`. Each ≤ 120 lines.
- **`SpawnAndRun` / `RunCmdAttach`** (`Target_Standalone/CliCommandRunner.cpp`) → already on the queue from the CLI-cleanup backlog; extract `WaitForReady`, `SendQuit`, `WaitForScenario` helpers. ~250 → ~80 lines each.

### C. Slice ordering

Slices are sized so each is one shippable PR; slice 1 lands the pattern doc + one canary refactor so the contract is concrete before the bulk work fans out.

1. **Slice 1 — pattern + canary** (small): write `docs/agent-rules/imgui-draw-pattern.md`. Pick one mid-sized victim (`DrawWhisperPreferencesTab`, ~779 lines, no cross-file deps) and refactor it as the canonical reference. Land both in one PR.
2. **Slice 2 — `ConfigManager::Load/Save` field table**. Self-contained, no UI thread risk, biggest dev-ergonomics win.
3. **Slice 3 — `McpPlugin::OnStart`**. Plugin-isolated, easy to test bucket-A.
4. **Slice 4 — `BuildUserFieldPayload`**. Pure logic, 75 branches → table dispatch. Already covered by `tests/Source_Core/TrackerFieldPayload.test.cpp`-style bucket-A; expand if gaps surface.
5. **Slice 5 — `HtmlToMarkdown` + `RegisterAiCommands`**. Mechanical, low-risk; bundled.
6. **Slice 6 — `main` + `AppController::Initialize`**. Bootstrap path. Sanitiser-run + dual-target build gate critical.
7. **Slice 7 — `AiAssistantController::RunRequest`**. Touches HTTP + streaming; needs the AI-driver bucket-E coverage to stay green.
8. **Slice 8 — `SpawnAndRun` + `RunCmdAttach`**. CLI-only, test via `tests/bats/cli_*.bats`.
9. **Slices 9-19 — ImGui draw monoliths**. One PR each, in descending order of pain (start with `drawActiveProjectWindow` once the pattern is battle-tested by slice 1). Group #11 (`drawMainMenuBar`) + #13 (`drawPreferencesWindow`) only if the diff stays under ~600 lines net.

Each ImGui-draw slice follows the same recipe:
1. Inventory existing `SMATCHET_UI_PERF_SCOPE` blocks → those become the section helpers.
2. Hoist `static` locals into a `<Foo>WindowState` member.
3. Extract action handlers into `OnX()` methods.
4. Update header with the new private API.
5. Rebuild + run the matching perf scenario before/after — baseline-bump only if intentional.

## Files to modify

Pattern doc + canary (slice 1):
1. `docs/agent-rules/imgui-draw-pattern.md` (new) — the canonical pattern reference.
2. `Source_Core/src/SmatchetPreferencesUi_Whisper.cpp:46` — canary refactor of `DrawWhisperPreferencesTab`.
3. `Source_Core/include/SmatchetPreferencesUi_Whisper.h` (or its existing header) — add the helper API + `WhisperPreferencesWindowState` struct.
4. `AGENTS.md` § Project rules — one-line cross-link to `imgui-draw-pattern.md` ("ImGui draw functions ≥ 200 lines must use the section-helper pattern — see `docs/agent-rules/imgui-draw-pattern.md`").

Non-UI monoliths (slices 2-8):
5. `Source_Core/src/ConfigManager.cpp:179,567` — field-registration table refactor.
6. `Source_Core/include/ConfigManager.h` — declare `FieldDesc` + table.
7. `Plugins/Mcp/McpPlugin.cpp:135` — split `OnStart`.
8. `Source_Core/src/TrackerFieldPayloadPure.cpp:179` — table-driven `BuildUserFieldPayload`.
9. `Source_Core/src/MarkdownConvert.cpp:1362` — tag-handler table for `HtmlToMarkdown`.
10. `Source_Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp:234` — `RegisterAiCommands` per-command-fn split.
11. `Source_Core/src/AiAssistantController.cpp:247` — phase-split `RunRequest`.
12. `Target_Standalone/main.cpp:232` — extract `BootApplication`/`RunFrameLoop`.
13. `Source_Core/src/AppController.cpp:1078` — split `Initialize` into 4 phases.
14. `Target_Standalone/CliCommandRunner.cpp:717,1182` — extract helpers from `SpawnAndRun` + `RunCmdAttach`.

ImGui draw monoliths (slices 9-19; one PR each):
15. `Source_Core/src/SmatchetActiveProjectGridUi.cpp:127` — `drawActiveProjectWindow` (992L).
16. `Source_Core/src/BlameAnalysisUi_Window.cpp:60` — `BlameAnalysisUi::DrawContent` (939L).
17. `Source_Core/src/SmatchetViewsDashboardUi.cpp:127` — `drawViewsDashboardWindow` (778L).
18. `Source_Core/src/SmatchetOfflineQueueUi.cpp:562` — `DrawUnifiedOfflineQueuesPanel` (608L).
19. `Source_Core/src/SmatchetUI.cpp:258` — `SmatchetUI::Draw` (589L).
20. `Source_Core/src/SmatchetPreferencesUi_Assistant.cpp:41` — `DrawAssistantPreferencesTab` (534L).
21. `Source_Core/src/SmatchetUI_MainMenu.cpp:52` — `drawMainMenuBar` (491L).
22. `Source_Core/src/SmatchetPreferencesUi.cpp:132` — `drawPreferencesWindow` (403L).
23. `Source_Core/src/SmatchetBulkTicketsUi.cpp:119` — `drawBulkImportWindow` (400L).
24. `Plugins/LuaConsole/LuaConsolePlugin.cpp:359` — `LuaConsolePlugin::OnDraw` (381L).
25. `Source_Core/src/TicketFieldEditor_Modal.cpp:177` — `RenderLongTextModal` (362L).

## Existing utilities reused

- `SMATCHET_UI_PERF_SCOPE` (`Source_Core/include/SmatchetPerfScope.h`) — already demarcates the natural section boundaries; pattern reuses them verbatim.
- `UiDrawSession` (`Source_Core/include/UiDrawSession.h`) — already the per-frame ctx carrier; `DrawCtx` extends, not replaces.
- Section-file naming precedent: `SmatchetViewsDashboardUi_widgets.cpp`, `SmatchetPreferencesUi_Whisper.cpp`, `SmatchetPreferencesUi_Assistant.cpp` (already-split files).
- `MakeCommand` (`Source_Core/include/Commands/Command.h`) — already supports per-command factory; `RegisterAiCommands` split just hoists per-command bodies into free functions.
- Bucket-A test pattern (`tests/Source_Core/*.test.cpp`) — pure-logic helpers extracted from `BuildUserFieldPayload`, `HtmlToMarkdown`, `ConfigManager::Load` become directly testable.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: zero-impact target. Perf scopes preserved by construction (extracted at existing `SMATCHET_UI_PERF_SCOPE` boundaries). Slice gate: per-slice baseline diff via `perf-gatekeeper`; any > 0.2 ms regression = revert.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: refactor is layout-only; no new sync I/O introduced. Existing async dispatchers (`MainThreadDispatcher`) unchanged.
- **Pillar 3 (never crash)**: each slice gated on dual-target build + sanitizer-clean (`ninja-debug-msvc` ASan/UBSan run for slices touching bootstrap or HTTP paths — slices 6, 7).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no change. The pattern preserves hotkey dispatch + tab order; no visual restyling.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

Per `docs/design/pillar-1-2-perf-review-system.md`.

1. **PR-fast CI** — each slice declares the matching scenario in its PR body. Mapping (from `agents/perf-gatekeeper.md` § Curated diff → scenario map):
   - Slice 1 (`SmatchetPreferencesUi_Whisper.cpp`) → `preferences-whisper-tab-render`.
   - Slice 2 (`ConfigManager.cpp`) → `app-cold-start` (Load is on the boot path).
   - Slice 3 (`McpPlugin.cpp`) → `mcp-server-startup`.
   - Slice 4 (`TrackerFieldPayloadPure.cpp`) → `bulk-payload-build-1000`.
   - Slices 9-19 ImGui draws → matching `<window>-render` scenario per window.
2. **Pillar 2 static scanner** — N/A; no new sync I/O reachable from `ImGui::*`. The refactor is layout reorganisation only.
3. **Dispatcher drain** — N/A; `MainThreadDispatcher::Drain()` not touched.
4. **Visible-cue bucket-E harness** — N/A; no new sync-stall path > 100 ms (slices 6 + 7 must double-check during implementation).
5. **Marker inventory** — perf scope **names** preserved by construction. If a slice introduces a new scope, regen `docs/perf/MARKER_INVENTORY.md` in that PR.

**Pre-push local check**: each slice runs the named scenario per `docs/PERF_WORKFLOW.md` § Gate-check vs baseline before opening the PR.

**Override**: `perf-out-of-band` label not anticipated — any slice that needs it has gone wrong; halt and re-slice.

## Risks / non-goals

- **Risk: ImGui-draw ordering bugs** — `ImGui::Begin/End` pairing, `PushID/PopID`, `BeginTable/EndTable` are positional. Mitigation: each slice runs bucket-E (ImGui Test Engine) screenshot diff against the pre-refactor golden; any visual delta = revert.
- **Risk: `static` local hoisting silently changes lifetime semantics** — `static` locals live across windows-of-same-type; member `<Foo>WindowState` is per-instance. For singletons (the common case in Smatchet UI) this is identical; for any future multi-instance window it's an improvement. Mitigation: grep + audit during slice 1; document in the pattern doc.
- **Risk: per-slice churn vs feature branches** — each slice will conflict with in-flight UI work. Mitigation: slices land in fast cadence (no slice waits more than 48 h once authored); feature branches rebase on the new shape.
- **Risk: clang-tidy / clang-format thrash** — extracting helpers can re-trigger `readability-function-cognitive-complexity` warnings on the new helpers. Mitigation: target ≤ 80 lines per helper, well under tidy thresholds.
- **Non-goal**: rewriting the underlying widget logic. Layout, behaviour, state semantics are byte-for-byte preserved. Pure mechanical decomposition.
- **Non-goal**: introducing a new UI framework / retained-mode layer. Smatchet stays on immediate-mode ImGui.
- **Non-goal**: enforcing the ≤ 200 lines / ≤ 30 branches budget via CI today. A `clang-tidy` config bump + grep gate are queued as follow-up work, separate plan.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where possible.

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - Slice 2 (`ConfigManager::Load/Save`) — add round-trip tests against the field table; one test per field-type.
  - Slice 4 (`BuildUserFieldPayload`) — expand existing `tests/Source_Core/TrackerFieldPayload*.test.cpp` to cover every dispatch-table entry.
  - Slice 5 (`HtmlToMarkdown`) — add `tests/Source_Core/MarkdownConvert.test.cpp` per tag-handler entry.
  - Slice 7 (`AiAssistantController::RunRequest`) — extract phase methods are bucket-A testable; add one test per phase.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: every ImGui-draw slice (9-19) gated on a screenshot-diff test against the pre-refactor golden. The window's existing scenario coverage is the source of truth; gaps = automation deferral noted in `docs/backlog/agent-self-improvement/test.md` per slice.
- **Bash-driver scenario / screenshot / sanitizer**:
  - Slices 6 + 7 — ASan/UBSan run via `cmake --build --preset ninja-debug-msvc-asan && scripts/dev/scenario-run.sh app-cold-start`.
  - Slice 8 — `tests/bats/cli_spawn.bats`, `tests/bats/cli_attach.bats`.
- **Build gate**: every slice — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Manual residue**: if a window has no existing bucket-E coverage, the slice ships the refactor + a `docs/backlog/agent-self-improvement/test.md` entry naming the missing scenario. No silent residue.

## Out of scope (flagged, not designed)

- **CI-enforced size cap** (line/branch budget in clang-tidy or a `test-lint-rules.sh` extension) — follow-up plan, depends on this one landing first so the baseline is reasonable.
- **Retained-mode UI layer** — explicitly rejected; ImGui stays.
- **Theming / accessibility audit** — orthogonal; Pillar 4 backlog owns it.
- **Refactoring functions below the cut (lines 21+ in the worst-functions ranking)** — re-rank after this plan ships; the long tail is short by definition once the head is dealt with.
- **`MarkdownPreviewRender` custom-deleter refactor** — already flagged out-of-scope by `policy-tighten-logging-raii.md`.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
