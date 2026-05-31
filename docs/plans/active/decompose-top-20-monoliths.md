# Plan — Decompose Top-20 Monolith Functions

> **Slug**: `decompose-top-20-monoliths`

> **⟳ Refreshed (re-validated against current `develop`).** The original plan was written against the pre-consolidation layout (`Source_Core/`, `Target_Standalone/`, `Plugins/`) — all its file paths + line numbers are stale, one target was renamed + split (`BlameAnalysisUi`→`AnnotateAnalysisUi`), and 2 targets fell under threshold (`SpawnAndRun` 249 / `RunCmdAttach` 219). A fresh measurement sweep confirms the **premise holds and worsened**: 21 of the 23 named functions are still >300-line monoliths (several *grew*), and the tree now has **30 functions >300 lines, not 20**. The ROI, however, is **mixed** (see § ROI) — so this refresh **re-sequences**: the CI size-cap gate **leads** (without it the work erodes — proven: `SmatchetUI::Draw` 589→608, `drawMainMenuBar` 491→504 since the plan was written), the genuinely-valuable non-UI table refactors are Phase A, and the mechanical ImGui-draw decompositions drop to **opportunistic / ride-along** (no dedicated mechanical sweep).

## Context

A line-count + branch-count sweep of `Source/Core/src/`, `Source/Plugins/`, `Source/Standalone/` (ThirdParty excluded) surfaces **30** functions over 300 lines each. The bulk are ImGui draw functions (immediate-mode UI naturally accretes); the rest are setup, config, payload-build, fetch, and HTTP-runner monoliths. Each:

- defeats `clang-tidy` cognitive-complexity warnings,
- forces full-file rereads on every minor edit,
- couples unrelated concerns (data fetch + layout + side-effect dispatch in one body),
- multiplies merge-conflict surface across feature branches,
- *(claimed)* inflates per-frame `SMATCHET_UI_PERF_SCOPE` parent-scopes — **but this benefit is largely illusory under the original approach** (it preserves existing scopes verbatim, so sub-widget cost stays invisible; and several targets have **zero** perf scopes anyway — see § Approach A).

Goal: no non-ThirdParty function exceeds **200 lines** / **30 branches**, enforced by a CI gate so it *stays* true, with a documented ImGui-draw pattern future authors copy by reflex.

## ROI (why this is re-scoped, not run whole)

- **🟢 Genuine ROI — non-UI table refactors (Phase A).** `ConfigManager` field-table, `BuildUserFieldPayload` dispatch, `HtmlToMarkdown` tag-handler table, `RegisterAiCommands` split. These kill real bug classes (config parallel-duplication on every key add; the 382-line payload branch-tower) **and** make the logic bucket-A testable — value *beyond* line count.
- **🔴 Low ROI as a sweep — the 12 ImGui-draw decompositions.** Pure mechanical, **no behaviour/feature value**; positional-ImGui (`Begin/End`, `PushID/PopID`) decomposition is regression-prone; conflicts with all in-flight UI work; the perf benefit isn't realised (scopes preserved); and **it regrows without the enforcement gate** (already observed). Doing 12 risky PRs whose result decays is net-negative. → made **ride-along** (decompose a draw fn only when already editing that window for a feature).
- **The gate is the keystone.** Land the line/branch cap in CI *first*; the rest is then either bug-reducing (Phase A) or free-rider (ride-along), and nothing erodes.

## Status — shipped & remaining (2026-05-31)

**Keystone + 9 decompositions shipped** (full detail in § Implementation log): Slice 0 gate (#627),
Slice 1a pattern doc (#630), and the function decompositions `drawActiveProjectWindow` 992→140 (#632),
`BuildValue` 169→24 (#633), `RegisterAiCommands` 325→7 (#634), `McpPlugin::OnStart` 687→77 (#635),
`HtmlToMarkdown` 337→67 (#639), `JiraClient::FetchFieldCatalog` 573→51 (#643), Plane
`FetchFieldCatalog`+`FetchIssuesStreamed` (#647). Gate is **live** — nothing regrows.

> **Everything below is grandfathered behind the gate** (the delta gate only fails NEW or
> just-crossed functions). Nothing here is blocking; each can land independently, in any order, as
> its own PR. The full current oversized set is snapshotted in
> [`docs/high-integrity/function-size-baseline.md`](../../high-integrity/function-size-baseline.md).
> **Cross-check the live baseline before starting any item — the plan's named symbol can drift**
> (Slice 4/8 both hit this). Expect a 2nd (sometimes 3rd) sub-extraction pass on any function with
> >~60 branches. Merge-discipline: with parallel decomposition PRs, **merge develop into the branch
> before merging** (CI's shallow clone defeats `git merge-base` → stale branches false-flag a
> sibling's now-decomposed function; see § Verification (actual) caveat + tooling.md P1).

**Remaining Phase A — genuine-ROI non-UI (the flagship set, recipes in § Approach B):**
- `ConfigManager::Load` (619 L/162 br) + `Save` (314 L/32 br), `Source/Core/src/Config/ConfigManager.cpp`
  — **highest value, highest risk.** Field-registration table (`FieldDesc kFields[]`); kills the
  config parallel-duplication bug class. Boot path → **silent config-corruption risk**: mandatory
  per-field round-trip tests (write non-default for every field, assert read-back) + a defaults test +
  the `app-cold-start` perf scenario. Do this one **serially, carefully, dedicated** — not fanned out.
- `AppController::Initialize` (432 L/61 br) — split `InitConfig`/`InitBackends`/`InitCommands`/`InitPlugins`. Bootstrap → ASan run.
- `main` (561 L/75 br, `Source/Standalone/main.cpp`) — extract `BootApplication`/`RunFrameLoop` (`ShutdownApplication` exists). Bootstrap → ASan.
- `AiAssistantController::RunRequest` (340 L/44 br) — phase-split fetch ↔ stream-parse ↔ history-update; each bucket-A testable. HTTP/streaming → keep AI-driver bucket-E green.

**Long tail surfaced by the 200-line cap** (NOT in the original 300-line sweep; all grandfathered):
- Command registrars — same per-command-fn split as `RegisterAiCommands` (#634), mechanical/low-risk,
  `command-system`: `RegisterDebugCommands` (315/23), `RegisterViewCommands` (295/32),
  `RegisterPerfCommands` (245/15), `RegisterTicketMutationCommands` (243/24).
- `AppController::SubmitFieldEdit` (257/43); `JiraClient::UpdateIssueFields` (243/61, `tracker-backend`).
- Sync (`offline-sync`): `OfflineQueueService::TickOfflineFieldEdits` (290/71),
  `TicketSyncService::TickStreamingApply` (276/43) — **perf-sensitive Tick paths; profile before/after.**
- `SpawnAndRun` (249/32) + `RunCmdAttach` (219/43), `Source/Standalone/CliCommandRunner.cpp` — the
  refresh banner called these "under threshold" at the old 300 cap; the 200 cap catches them.

**Phase B — ImGui-draw monoliths: RIDE-ALONG ONLY (no dedicated PRs).** The ~15 UI draws still > 200 L
(`SmatchetUI::Draw`, `drawMainMenuBar`, `DrawWhisperPreferencesTab`, `drawViewsDashboardWindow`,
`AnnotateAnalysisUi::DrawContent`, `DrawUnifiedOfflineQueuesPanel`, `LuaConsolePlugin::OnDraw`, …; full
list in § Files Phase B + the baseline snapshot). Decompose one **only when a feature already opens that
file**, using § Approach A + [`docs/guides/imgui-draw-pattern.md`](../../guides/imgui-draw-pattern.md).
The canary (#632) is the worked reference.

## Approach

### A. ImGui draw-function pattern (canonical)

Authors today write a single `void DrawX(...)` that owns window setup, per-section layout, every state mutation, and every action dispatch. The pattern below extracts at section seams.

> **⚠ Seam-premise correction (refresh).** The original plan asserted the seam is "the existing `SMATCHET_UI_PERF_SCOPE` brace-blocks — already the de-facto section boundaries." That is **only true where the scopes exist**. Verified counts: `SmatchetUI.cpp` 27 scopes ✓, `SmatchetActiveProjectGridUi.cpp` 8 ✓ — but **`SmatchetViewsDashboardUi.cpp` (778 L) and `SmatchetPreferencesUi_Whisper.cpp` (779 L, the original slice-1 canary) have ZERO**. For those, there is no existing seam to reuse: the author invents section boundaries and (if perf coverage is wanted) *adds* scopes — which **does** shift the baseline + needs a `MARKER_INVENTORY.md` regen. So: **where perf scopes already bracket sections, reuse them verbatim (zero baseline shift); otherwise decompose on logical section boundaries and treat any new scope as an intentional, inventory-tracked addition.** The canary is changed to a scope-bearing function (see § Slice ordering).

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

Rules of the pattern (codified in `docs/guides/imgui-draw-pattern.md`, added by this plan):

1. **`DrawCtx` struct** holds the per-frame snapshots + references. No more 30-line argument lists; no more `static` locals leaking across windows.
2. **One responsibility per helper**. Header / body / footer / modals / hotkeys are non-overlapping. A helper that grows past ~80 lines splits again.
3. **Perf scopes stay at the section boundary** *where they exist* (reuse verbatim → zero baseline-bump churn). Where a function has no scopes (see § seam-premise correction), decompose on logical sections; add a scope only intentionally + regen `MARKER_INVENTORY.md`. Helper-internal scopes only if `perf-detective` asks for finer resolution.
4. **Window-state extraction**. Persistent `static` locals (filter buffers, expanded-row sets, last-selection ids) move into a `<Foo>WindowState` member struct on the owning UI object. Caught by `grep "static.*Buf\|static bool s_" Source/Core/src/Smatchet*Ui*.cpp`.
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
- **`main` (`Source/Standalone/main.cpp`)** → already partially decomposed; extract remaining `// Boot phase` blocks into `BootApplication()`, `RunFrameLoop()`, `ShutdownApplication()` (`ShutdownApplication` already exists). Goal: `main` ≤ 80 lines.
- **`AppController::Initialize`** → split into `InitConfig()`, `InitBackends()`, `InitCommands()`, `InitPlugins()`. Each ≤ 120 lines.
- ~~**`SpawnAndRun` / `RunCmdAttach`**~~ — *dropped (refresh): now 249 / 219 lines, under the 300 threshold. Not monoliths; the Slice-0 gate keeps them honest.*

### C. Slice ordering

Re-sequenced (refresh): the **gate leads**, the **bug-reducing Phase A** follows, and UI decomposition is **ride-along, not a fanned-out sweep**.

**Slice 0 — CI size-cap gate (the keystone; was "out of scope", now leads).** Extend `agents/scripts/project/test-lint-rules.sh` (or a sibling) with a `function-too-long` / `function-too-branchy` rule, **delta-gated** (only NEW or grown functions fail; the existing 30 are grandfathered into `docs/high-integrity/baseline.md`, same mechanism as the comment-bloat rules). Without this, every decomposition below regrows. Pairs with the pattern doc. *Pure-logic; no `Source/` change → fast CI.*

**Slice 1 — pattern doc + a scope-bearing canary** (small). Write `docs/guides/imgui-draw-pattern.md`; refactor **`drawActiveProjectWindow`** (992 L, **8 existing `SMATCHET_UI_PERF_SCOPE` blocks** → real reusable seams, zero baseline shift) as the canonical reference. *(Original canary `DrawWhisperPreferencesTab` is rejected — it has zero perf scopes, so it can't demonstrate the "reuse existing seams" contract.)*

**Phase A — non-UI table refactors (genuine ROI; ship these regardless of the UI work).**
2. **`ConfigManager::Load/Save` field table** (`Load` 620 L, `Save` 315 L). Self-contained, no UI-thread risk, kills the parallel-duplication bug class on every config-key add. Biggest dev-ergonomics + correctness win.
3. **`McpPlugin::OnStart`** (687 L) → `RegisterCoreRoutes/ToolRoutes/SseRoutes/StartServer`. Plugin-isolated, bucket-A testable.
4. **`BuildUserFieldPayload`** (382 L) → `{FieldType → builder fn}` dispatch. Pure logic; expand `tests/Core/Tracker/TrackerFieldPayload*.test.cpp`.
5. **`HtmlToMarkdown`** (337 L) tag-handler table **+ `RegisterAiCommands`** (325 L) per-command-fn split. Mechanical, low-risk; bundled.
6. **`main`** (556 L) **+ `AppController::Initialize`** (432 L). Bootstrap path — sanitiser run + dual-target build gate critical.
7. **`AiAssistantController::RunRequest`** (340 L) → fetch ↔ stream-parse ↔ history-update phases. HTTP + streaming; keep AI-driver bucket-E green.

(`FetchFieldCatalog` 573 L, `FetchIssuesStreamed` 390 L are *new* non-UI monoliths surfaced by the refresh — table/phase-split candidates; fold into Phase A if touched, else they're caught by the Slice-0 gate going forward.)

**Phase B — ImGui-draw decompositions: RIDE-ALONG, not dedicated PRs.** Do **not** open a mechanical PR per draw fn. Instead, when a feature already opens one of these files, decompose that function as part of that PR using § Approach A. Rationale: the marginal cost is ~zero (you're editing it anyway), it avoids the churn/conflict/regression cost of a sweep, and the Slice-0 gate prevents regrowth. Current ride-along candidates (descending size; see § Files to modify for paths/lines): `drawActiveProjectWindow`(992, done as the canary), `AnnotateAnalysisUi::DrawContent`(945), `DrawWhisperPreferencesTab`(779), `drawViewsDashboardWindow`(778), `SmatchetUI::Draw`(608), `DrawUnifiedOfflineQueuesPanel`(606), `DrawAssistantPreferencesTab`(531), `drawMainMenuBar`(504), `DrawTemplatePreferencesTabs`(504), `RenderNewIssueDraftRow`(464), `DrawGridHeaderToolbar`(447), `drawPreferencesWindow`(403), `drawBulkImportWindow`(400), `RenderFieldCell`(382), `LuaConsolePlugin::OnDraw`(381), `RenderLongTextModal`(362). *(`SpawnAndRun` 249 / `RunCmdAttach` 219 dropped — now under threshold.)*

Each draw-fn ride-along follows the same recipe:
1. **If** the function already has `SMATCHET_UI_PERF_SCOPE` blocks → those become the section helpers (zero baseline shift). **Else** decompose on logical sections; any new scope is an intentional `MARKER_INVENTORY.md`-tracked add.
2. Hoist `static` locals into a `<Foo>WindowState` member.
3. Extract action handlers into `OnX()` methods.
4. Update header with the new private API.
5. Rebuild + run the matching perf scenario before/after — baseline-bump only if intentional.

## Files to modify

> Paths + line numbers re-measured against current `develop` (refresh). Line numbers drift — treat them as locators, not contracts.

Gate + pattern (slices 0-1):
1. `agents/scripts/project/test-lint-rules.sh` (+ `docs/high-integrity/baseline.md` grandfather snapshot) — add the delta-gated `function-too-long` / `function-too-branchy` rule (Slice 0).
2. `docs/guides/imgui-draw-pattern.md` (new) — the canonical pattern reference (Slice 1).
3. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp:127` — canary refactor of `drawActiveProjectWindow` (992 L, 8 perf scopes) + its header — add helper API + `ActiveProjectWindowState`.
4. `AGENTS.md` § Project rules — one-line cross-link ("ImGui draw functions ≥ 200 lines use the section-helper pattern — see `docs/guides/imgui-draw-pattern.md`; enforced by the function-size gate").

Phase A — non-UI monoliths (slices 2-7):
5. `Source/Core/src/Config/ConfigManager.cpp:606` (`Load` 620 L) + `:175` (`Save` 315 L) + its header — field-registration table (`FieldDesc`).
6. `Source/Plugins/Mcp/McpPlugin.cpp:134` — split `OnStart` (687 L).
7. `Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp:179` — table-driven `BuildUserFieldPayload` (382 L).
8. `Source/Core/src/Ui/MarkdownConvert.cpp:1354` — tag-handler table for `HtmlToMarkdown` (337 L).
9. `Source/Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp:231` — `RegisterAiCommands` per-command-fn split (325 L).
10. `Source/Core/src/AiAssistantController.cpp:247` — phase-split `RunRequest` (340 L).
11. `Source/Standalone/main.cpp:235` — extract `BootApplication`/`RunFrameLoop` (556 L).
12. `Source/Core/src/AppController.cpp:1084` — split `Initialize` into phases (432 L).
13. *(new, optional Phase A)* `Source/Core/src/Tracker/TrackerFieldCatalog.cpp:77` `FetchFieldCatalog` (573 L); `Source/Core/src/Tracker/PlaneIssueSearch.cpp:112` `FetchIssuesStreamed` (390 L).

Phase B — ImGui-draw monoliths (**ride-along only — no dedicated PR**; touch when already in the file):
14. `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:60` — `AnnotateAnalysisUi::DrawContent` (945 L). *(was `BlameAnalysisUi_Window.cpp` / `BlameAnalysisUi::DrawContent`, renamed + split per rename-blame-to-annotate.)*
15. `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp:46` — `DrawWhisperPreferencesTab` (779 L; **0 perf scopes** → invent seams).
16. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:127` — `drawViewsDashboardWindow` (778 L; **0 perf scopes**).
17. `Source/Core/src/Ui/SmatchetUI.cpp:260` — `SmatchetUI::Draw` (608 L; 27 perf scopes; *grew* from 589).
18. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp:562` — `DrawUnifiedOfflineQueuesPanel` (606 L).
19. `Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp:41` — `DrawAssistantPreferencesTab` (531 L).
20. `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:51` — `drawMainMenuBar` (504 L; *grew* from 491).
21. `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp:28` — `DrawTemplatePreferencesTabs` (504 L; *new*).
22. `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:154` — `RenderNewIssueDraftRow` (464 L; *new*).
23. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:42` — `DrawGridHeaderToolbar` (447 L; *new*).
24. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:132` — `drawPreferencesWindow` (403 L).
25. `Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp:119` — `drawBulkImportWindow` (400 L).
26. `Source/Core/src/TicketFieldEditor.cpp:866` — `RenderFieldCell` (382 L; *new*).
27. `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:359` — `LuaConsolePlugin::OnDraw` (381 L).
28. `Source/Core/src/TicketFieldEditor_Modal.cpp:177` — `RenderLongTextModal` (362 L).

*(Dropped: `SpawnAndRun` 249 L / `RunCmdAttach` 219 L in `Source/Standalone/CliCommandRunner.cpp` — now under the 300 threshold; not monoliths.)*

## Existing utilities reused

- `SMATCHET_UI_PERF_SCOPE` (`Source/Core/include/UiPerfMonitor.h`) — already demarcates the natural section boundaries; pattern reuses them verbatim.
- `UiDrawSession` (`Source/Core/include/SmatchetUiSession.h`) — already the per-frame ctx carrier; `DrawCtx` extends, not replaces.
- Section-file naming precedent: `SmatchetViewsDashboardUi_widgets.cpp`, `SmatchetPreferencesUi_Whisper.cpp`, `SmatchetPreferencesUi_Assistant.cpp` (already-split files).
- `MakeCommand` (`Source/Core/include/Commands/Command.h`) — already supports per-command factory; `RegisterAiCommands` split just hoists per-command bodies into free functions.
- Bucket-A test pattern (`tests/Core/*.test.cpp`) — pure-logic helpers extracted from `BuildUserFieldPayload`, `HtmlToMarkdown`, `ConfigManager::Load` become directly testable.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: zero-impact target *where existing perf scopes are reused verbatim*; where a function has none and a scope is added, that's an intentional, inventory-tracked baseline change (not silent — see § seam-premise correction). Slice gate: per-slice baseline diff via `perf-gatekeeper`; any > 0.2 ms regression = revert.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: refactor is layout-only; no new sync I/O introduced. Existing async dispatchers (`MainThreadDispatcher`) unchanged.
- **Pillar 3 (never crash)**: each slice gated on dual-target build + sanitizer-clean (`ninja-debug-msvc` ASan/UBSan run for slices touching bootstrap or HTTP paths — slices 6, 7).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no change. The pattern preserves hotkey dispatch + tab order; no visual restyling.

## Perf-gate section (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`.

1. **PR-fast CI** — each slice declares the matching scenario in its PR body. Mapping (from `agents/core/perf-gatekeeper.md` § Curated diff → scenario map):
   - Slice 1 canary (`SmatchetActiveProjectGridUi.cpp`) → the `active-project-window-render` scenario.
   - Slice 2 (`ConfigManager.cpp`) → `app-cold-start` (Load is on the boot path).
   - Slice 3 (`McpPlugin.cpp`) → `mcp-server-startup`.
   - Slice 4 (`TrackerFieldPayloadPure.cpp`) → `bulk-payload-build-1000`.
   - Phase B ride-along draws → the matching `<window>-render` scenario for that window.
2. **Pillar 2 static scanner** — N/A; no new sync I/O reachable from `ImGui::*`. The refactor is layout reorganisation only.
3. **Dispatcher drain** — N/A; `MainThreadDispatcher::Drain()` not touched.
4. **Visible-cue bucket-E harness** — N/A; no new sync-stall path > 100 ms (slices 6 + 7 must double-check during implementation).
5. **Marker inventory** — perf scope **names** preserved by construction. If a slice introduces a new scope, regen `docs/perf/MARKER_INVENTORY.md` in that PR.

**Pre-push local check**: each slice runs the named scenario per `docs/guides/perf-workflow.md` § Gate-check vs baseline before opening the PR.

**Override**: `perf-out-of-band` label not anticipated — any slice that needs it has gone wrong; halt and re-slice.

## Risks / non-goals

- **Risk: ImGui-draw ordering bugs** — `ImGui::Begin/End` pairing, `PushID/PopID`, `BeginTable/EndTable` are positional. Mitigation: each slice runs bucket-E (ImGui Test Engine) screenshot diff against the pre-refactor golden; any visual delta = revert.
- **Risk: `static` local hoisting silently changes lifetime semantics** — `static` locals live across windows-of-same-type; member `<Foo>WindowState` is per-instance. For singletons (the common case in Smatchet UI) this is identical; for any future multi-instance window it's an improvement. Mitigation: grep + audit during slice 1; document in the pattern doc.
- **Risk (the big one): erosion without the gate.** Pure decomposition reverts under feature pressure — *already observed* (`SmatchetUI::Draw` 589→608, `drawMainMenuBar` 491→504 since this plan was written). Mitigation: **Slice 0 ships the delta-gated size cap first**; everything after is protected. This is why the gate moved from "out of scope" to slice 0.
- **Risk: per-slice churn vs feature branches** — a dedicated UI-decomposition sweep conflicts with all in-flight UI work. Mitigation: **don't sweep** — Phase B is ride-along (you're already in the file). Phase A files are non-UI / low-churn.
- **Risk: seam absence** — not every draw fn has `SMATCHET_UI_PERF_SCOPE` blocks (e.g. Whisper, ViewsDashboard have none). Mitigation: § Approach A seam-premise correction — invent logical seams; any new scope is an intentional `MARKER_INVENTORY.md` add (not a silent baseline shift).
- **Risk: clang-tidy / clang-format thrash** — extracted helpers can re-trigger `readability-function-cognitive-complexity`. Mitigation: target ≤ 80 lines per helper.
- **Non-goal**: rewriting widget logic. Layout/behaviour/state semantics byte-for-byte preserved. Pure mechanical decomposition.
- **Non-goal**: introducing a new UI framework / retained-mode layer. Smatchet stays immediate-mode ImGui.
- **(Was a non-goal, now Slice 0 — IN scope)**: the ≤ 200-line / ≤ 30-branch CI cap. It was deferred in the original plan; the refresh promotes it to the **leading** slice because without it the decomposition decays.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where possible.

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - Slice 2 (`ConfigManager::Load/Save`) — add round-trip tests against the field table; one test per field-type.
  - Slice 4 (`BuildUserFieldPayload`) — expand existing `tests/Core/TrackerFieldPayload*.test.cpp` to cover every dispatch-table entry.
  - Slice 5 (`HtmlToMarkdown`) — add `tests/Core/MarkdownConvert.test.cpp` per tag-handler entry.
  - Slice 7 (`AiAssistantController::RunRequest`) — extract phase methods are bucket-A testable; add one test per phase.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: every Phase-B ride-along draw decomposition gated on a screenshot-diff test against the pre-refactor golden. The window's existing scenario coverage is the source of truth; gaps = automation deferral noted in `docs/self-improvement/categories/test.md` for that PR.
- **Bash-driver scenario / sanitizer**:
  - Slices 6 + 7 (`main`/`Initialize`, `RunRequest`) — ASan/UBSan run on the bootstrap/HTTP paths (`ninja-*-asan` preset + the matching scenario via `scripts/dev/perf-run.sh` / `scenario.run`).
  - *(CliCommandRunner `cli_*.bats` slice dropped — `SpawnAndRun`/`RunCmdAttach` now under threshold.)*
- **Build gate**: every slice — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Manual residue**: if a window has no existing bucket-E coverage, the slice ships the refactor + a `docs/self-improvement/categories/test.md` entry naming the missing scenario. No silent residue.

## Out of scope (flagged, not designed)

- ~~**CI-enforced size cap** — follow-up plan, depends on this one landing first.~~ **Moved IN scope as Slice 0** (refresh): the gate must *lead*, not follow, or the decomposition decays (see § ROI / § Risks).
- **Retained-mode UI layer** — explicitly rejected; ImGui stays.
- **Theming / accessibility audit** — orthogonal; Pillar 4 backlog owns it.
- **Refactoring functions below the cut (lines 21+ in the worst-functions ranking)** — re-rank after this plan ships; the long tail is short by definition once the head is dealt with.
- **`MarkdownPreviewRender` custom-deleter refactor** — already flagged out-of-scope by `policy-tighten-logging-raii.md`.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

- **Slice 0 — CI size-cap gate (the keystone).** `agents/scripts/core/function_size_audit.py` (new):
  heuristic C++ function extractor — comment/string-neutralizing "skeleton" pass, brace-depth
  walker with a backward classifier rejecting control blocks / lambdas / scopes / aggregate
  inits. Two delta-gated rule-ids `function-too-long` (> 200 lines) / `function-too-branchy`
  (> 30 decision points), keyed `(rule, basename, qualified-name)`, diffed HEAD vs merge-base of
  `origin/develop` so the existing monoliths are grandfathered. Wired into
  `agents/scripts/project/test-lint-rules.sh` diff mode (third gate block, parallel to the
  comment-regrowth gate; fail-closed on infra error; `SMATCHET_DEVIATION` suppression). New modes
  `--funcsize-baseline` (writes the informational snapshot) + `--scan-file` (git-free, for tests);
  `--selftest` extended to assert the two rule-ids are documented in `AGENTS.md`. Snapshot
  `docs/high-integrity/function-size-baseline.md` (47 too-long + branchy grandfathered). Tests:
  `tests/bats/function_size.bats` (6) + fixtures `tests/fixtures/function_size/`. AGENTS.md
  § Tiered enforcement documents the rules. Parser validated: line counts match this plan's own
  measurements within ±1–5 lines (`drawActiveProjectWindow` 992, `Draw` 608, `DrawWhisper…` 779).
  - **CodeRabbit round (PR #627):** 4 actionable findings fixed in-band — (1) operator-overload
    name parsing (`operator()` / `operator[]` / `operator bool` were silently skipped → now
    detected via `_name_before_paren`); (2) backslash-continued preprocessor lines (`#define … \`)
    now stay skipped across the splice so macro braces can't perturb depth; (3) identity key
    extended to `(rule, basename, qualified-name, arity)` so same-named overloads no longer
    collapse / mis-grandfather; (4) `_suppressed` parses comma-separated rule ids so a dual-cap
    function is suppressible by one `SMATCHET_DEVIATION(rule=function-too-long,function-too-branchy)`.
    Added fixtures + 4 bats cases (operator detection, dual-cap fires both, comma-deviation
    suppresses both). Also resolved an AGENTS.md merge conflict from #626 (plan path active→shipped).
- **Slice 1a — ImGui draw-function pattern doc (PR #630).** `docs/guides/imgui-draw-pattern.md`
  (new): canonical `DrawCtx` + section-helper shape, the 6 § Approach-A rules, a positional-ImGui
  hazards section (Begin/End + PushID/PopID pairing, byte-for-byte layout preservation, bucket-C/E
  golden verification), `drawActiveProjectWindow` named as the worked canary. `AGENTS.md`
  § Project rules cross-link + ride-along-only note. Pure-docs.
- **Slice 1b — `drawActiveProjectWindow` canary (PR #632, `grid-engine`).** 992→~140 L. Extracted
  11 section helpers + 3 anon-namespace free helpers at the 8 `activeProject:*` perf-scope seams
  (scope names verbatim — zero baseline shift; the row-emitting helpers correctly nested *inside*
  the `BeginTable`/`EndTable` scope), `ActiveProjectDrawCtx` struct, and an `ActiveProjectWindowState`
  member hoisting the two former `static` locals. Orchestrator-verified: objective paired-ImGui
  balance identical to develop (Begin/End, BeginTable/EndTable, BeginChild/EndChild, PushID/PopID).
  CI: **Bucket-C screenshot diff + Bucket-E + ASan + Perf-PR-fast all PASS** (the plan-mandated
  visual/interaction/sanitizer/perf gate). `tests-out-of-band` (visual regression IS the test).
- **Slice 3 — `McpPlugin::OnStart` (PR #635, `mcp-toolsmith`).** 687→77 L. 8 registration-phase
  helpers (`Authorize`, `RegisterTicketRoutes`, `RegisterToolsListRoute`, `RegisterToolsCallRoute`,
  `RegisterSseRoute`, `RegisterJsonRpcRoutes`, `HandleJsonRpcToolsCall` [2nd-pass sub-extraction to
  clear the 30-branch cap], `StartServerThread`). Route-registration order byte-identical; preprocessor
  guards balanced (proven by dual-target build 704/704 exit 0). `tests-out-of-band` (plugin lifecycle).
- **Slice 4 — `BuildValue` table-dispatch (PR #633, `tracker-backend`).** 169→24 L dispatcher +
  `kScalarBuilders` table + 11 per-type scalar builders + array/labels builders. **Test delta added**
  (2 TEST_CASEs covering every dispatch entry + a precedence test; corrected 3 wrong expectations to
  preserve original option/component/priority precedence). 17 cases / 186 assertions PASS.
- **Slice 5 — `RegisterAiCommands` (PR #634, `command-system`).** 325→7 L dispatcher + 5 per-command
  free functions (`RegisterListModels/DumpRequest/Probe/SendOnce/ValidatePrefs Command`). Command-name
  set + registration order byte-identical. `tests-out-of-band` (registry introspection needs the full
  AppController/AI dep chain not linked in `SmatchetTests`). The `HtmlToMarkdown` half of plan Slice 5
  shipped as Slice 5b below.
- **Slice 5b — `HtmlToMarkdown` (PR #639, general agent).** 337→**67 L / 12 br** in a single pass (no
  2nd extraction). `HtmlMdCtx` struct holds shared state (former in-fn lambdas → methods); two
  `unordered_map` dispatch tables key tag-name → free handler (25 handlers: open/void/close). **Test
  delta**: new `tests/Core/MarkdownConvert.test.cpp` (24 cases / 48 assertions, one group per tag,
  goldens captured pre-refactor → 48/48 byte-for-byte). Build + gate PASS.
- **Slice 8 — `JiraClient::FetchFieldCatalog` (PR #643, `tracker-backend`).** 573 L / 165 br → **51-line**
  fetch→parse→enrich→finalize orchestrator. 6 HTTP-phase helpers (`FetchAndParseFieldList`,
  `EnrichGlobalCatalogFields`, `EnrichFromCreateMeta`, `EnrichIssueTypeFromProject`,
  `DiscoverSprintBoardIds`, `EnrichSprintFields`) + 7 pure mappers in the existing
  `TrackerFieldCatalogPure` seam (no `ITrackerClient` interface delta). **3 extraction passes** (createmeta
  needed a 3rd-pass sub-split for the branch budget). **Test delta**: +21 cases (26/176 assertions PASS).

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

- **Grandfather snapshot lives in its OWN file** (`docs/high-integrity/function-size-baseline.md`),
  not co-mingled into `docs/high-integrity/baseline.md` as the plan's § Files-to-modify row 1
  implied. Rationale: the strict `baseline.md` has a byte-determinism contract (a bats test diffs
  it); injecting the funcsize set would couple two independent gates and risk that test. The gate
  itself is a live merge-base delta (not the file), so the snapshot is purely informational —
  identical role to the comment-regrowth rules, which also keep no entry in `baseline.md`.
- **Grandfather granularity is function-identity, not line-count.** A function fires only when it
  is NEW over a cap or has just crossed it; a grandfathered 600-line function growing to 650 stays
  grandfathered. This matches the plan's "same mechanism as the comment-bloat rules" wording. It
  does NOT re-flag erosion *within* an already-oversized function — that protection applies once a
  slice brings the function under 200 (then any later regrowth past 200 re-fires).
- **Function detector is a heuristic** (brace scanner, not libclang — unavailable/non-portable).
  Systematic quirks cancel in the HEAD-vs-base set-diff for unchanged code; only genuinely new /
  just-crossed functions are flagged, and `SMATCHET_DEVIATION` is the escape hatch.
- **Slice 1 split into 1a (pattern doc) + 1b (canary).** The plan paired the doc with the
  992-line `drawActiveProjectWindow` decomposition in one slice. Shipped the doc first (1a, PR
  #630, pure-docs — immediately useful, unblocks Phase B); the canary surgery (1b) follows as its
  own build-gated PR with CI bucket-C/E screenshot-diff verification. Rationale: a positional-ImGui
  decomposition of that size carries real visual-regression risk and a ~20-min dual-target build —
  far safer isolated than bundled with docs. Delegated to `grid-engine`.
- **Slice 4 target was misnamed in the plan.** § Approach B / § Files row 7 name `BuildUserFieldPayload`,
  but that function is already ~33 L; the actual flagged monolith is `BuildValue` (169 L/55 br) in the
  same file (`TrackerFieldPayloadPure.cpp`) — and `BuildValue` *is* the field-type-dispatch tower the
  plan describes. `tracker-backend` caught this via a baseline cross-check. **Standing correction:
  decompose targets must be verified against the live `function-size-baseline.md`, not the plan's named
  symbol** (the symbol can drift). Every subsequent slice now does this cross-check first.
- **Branchy seams need a second-pass extraction** (confirmed 2×). The § Approach A "one helper per
  perf-scope seam" recipe is sound but not single-pass for the *branchier* monoliths: the canary's
  `grid.sort`/`grid.rows`/`grid.rectSel.keys` helpers and Slice 3's `RegisterJsonRpcRoutes` each tripped
  the 30-branch cap on first extraction and needed a sub-extraction (cell/rect-sel helpers; a
  `HandleJsonRpcToolsCall` split). Expect a 2nd pass when decomposing a function with > ~60 total branches.
- **`HtmlToMarkdown` half of Slice 5 deferred** to a follow-up; `RegisterAiCommands` shipped alone.
  Shipped as Slice 5b (#639).
- **`FetchFieldCatalog` is a phase-split, not a `{schema-type → mapper}` table** (Slice 8). § Approach B's
  table recipe assumed a per-type dispatch switch; the function's actual shape is a *sequence of
  per-endpoint enrichment phases* (field-list → createmeta → project issue-type → sprint boards →
  sprint fields) with no single type-keyed branch point to tabularize. Split along the real phase
  boundaries (the plan's own primary recommendation) — behaviour-preserving, gate-confirmed. Standing
  note: pick table-vs-phase per the function's actual branch structure, not the plan's blanket recipe.

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

- **`tests/bats/function_size.bats` — 6/6 PASS**: `function-too-long` + `function-too-branchy`
  detection; known-good negative (aggregate-init + lambda not misdetected); `--diff` delta gate
  new-function-fails / grandfathered-passes / `SMATCHET_DEVIATION`-suppresses (throwaway git repos).
- **`tests/bats/lint_rules.bats` — regression** re-run after the wiring change.
- **`--selftest` PASS** — rule-ids in sync with AGENTS.md.
- **Live gate** `test-lint-rules.sh --diff origin/develop` — function-size block reports PASS on the
  unchanged tree (zero new; all 47+ monoliths grandfathered).
- **Pure-tooling slice** (shell + python + fixtures + docs) — no `Source/` change, so no
  `cmake --build` / perf scenario (per `AGENTS.md` § Cadence; perf-gate N/A — no C++ touched).

**Slices 1b / 3 / 4 / 5 (shipped — full CI green on each):**
- **Slice 1b canary (#632)** — Windows MSVC + light + Coverage PASS; **Bucket-C screenshot diff PASS**
  (pixel-identical) + **Bucket-E UI tests PASS** (interaction preserved) + **Sanitizer/ASan PASS** +
  **Perf-PR-fast PASS** (no regression). This is the plan's full Phase-B verification bar.
- **Slice 4 BuildValue (#633)** — dual-target build PASS; ctest 17 cases / 186 assertions PASS (test
  delta); ASan + Perf + Coverage + bucket-C/E PASS.
- **Slice 5 RegisterAiCommands (#634)** + **Slice 3 OnStart (#635)** — dual-target build PASS; full CI
  green (MSVC + light + Coverage + Perf + Sanitizer + bucket-C/E); `tests-out-of-band` (justified).
- **Post-merge sanity**: `function_size_audit.py --list` on develop confirms all four functions
  (`drawActiveProjectWindow`, `BuildValue`, `RegisterAiCommands`, `McpPlugin::OnStart`) are out of the
  oversized set.
- **CI gate-staleness caveat** (filed as follow-up): a PR branched *before* a decomposition landed on
  develop false-flags the now-decomposed function (`function-too-long`/`comment-*`) because CI's
  shallow clone defeats `git merge-base`, so the delta gate compares against tip-of-develop, not the
  PR's fork point. Hit on #633 (drawActiveProjectWindow flagged). Workaround used: merge develop into
  the branch before merge. Real fix: CI checkout `fetch-depth: 0` (or deepen to the merge-base). Backlog:
  `docs/self-improvement/categories/tooling.md`.
