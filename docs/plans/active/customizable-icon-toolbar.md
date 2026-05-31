# Plan — Customizable icon toolbar (Total-Commander-style button bar)

> **Slug**: `customizable-icon-toolbar` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan-doc family, § Process rules.

## Context

Smatchet exposes actions only through the hardcoded main menu bar
(`Source/Core/src/Ui/SmatchetUI_MainMenu.cpp`) and the inline command palette.
There is no fast, always-visible, user-customizable action strip. Total Commander's
*Button bar* (https://www.ghisler.ch/wiki/index.php?title=Button_bar) is the model:
a row of icon buttons under the menu, each bound to an action, fully user-editable
(add / remove / reorder / edit) with separators, persisted across sessions.

Prompted by user request. Intended outcome — **after this lands, a user sees a
customizable Font-Awesome icon toolbar directly below the menu bar; each button runs
a registered command or a Lua snippet; the bar is a global base set plus per-tracker
appended buttons, all editable via a dialog + right-click and persisted.**

Confirmed decisions: action types = **Command + Lua** (+ Separator); customization =
**dialog + right-click**; scope = **global base always shown + each tracker appends
its own buttons at the end**; icons = **Font Awesome only**.

## Approach

Add one render widget pinned under the menu bar via the existing
`ImGui::BeginViewportSideBar(ImGuiDir_Up, …)` pattern (same mechanism the status bar
already uses with `ImGuiDir_Down`), an editor dialog, and an icon picker. The toolbar
reuses the unified command system for clicks, `ConfigManager` for persistence, and the
already-loaded Font Awesome atlas for icons — **no new subsystems**. A button is a
small data record persisted as JSON; the bar is the global list concatenated with the
current tracker's append list. The only non-obvious trade-off: panel-open actions
(palette / settings / new-view) are UI-flag toggles today, not commands, so a tiny
`ui.*` command family is added to keep every toolbar action uniform (a command id) —
this also benefits palette/CLI/MCP.

### Data model (new header `Source/Core/include/Config/ToolbarConfig.h`)

Pure data — only `<string>`, `<vector>`, nlohmann json; **no ImGui** so the Config layer
stays UI-free and dual-target clean. Provide `to_json` / `from_json` free functions.

```cpp
enum class ToolbarButtonKind { Command, Lua, Separator };

struct ToolbarButton {
  ToolbarButtonKind Kind = ToolbarButtonKind::Command;
  std::string IconName;   // stable FA name token, e.g. "magnifying-glass" (picker/JSON readability)
  std::string IconGlyph;  // resolved UTF-8 glyph (source of truth; renders even if catalog missing)
  std::string Tooltip;    // hover text
  std::string CommandId;  // Kind==Command
  nlohmann::json Args = nlohmann::json::object();  // Kind==Command; defaults to {} so dispatch never sees null
  std::string LuaCode;    // Kind==Lua
};

struct ToolbarConfig {
  bool Visible = true;
  std::vector<ToolbarButton> Buttons;
  static ToolbarConfig Default();  // curated starter set (see § Default buttons)
};
```

Per-tracker append — extend `ViewWorkspaceState` (`ConfigManager.h:408`) with
`std::vector<ToolbarButton> ToolbarAppend;`.

**Effective-toolbar resolution** (pure, unit-testable): `effective = global.Buttons`;
if the current tracker's `ToolbarAppend` is non-empty, append `[Separator] + ToolbarAppend`,
then **collapse adjacent separators and trim any leading/trailing separator** so the bar never
shows doubled or dangling dividers (covers the case where global already ends in a separator or
the append list starts with one); no backend connected → global only. Resolved once per frame.

### Rendering

```cpp
ImGuiViewport* vp = ImGui::GetMainViewport();
const float h = ImGui::GetFrameHeight() + style.FramePadding.y * 2.0f;
if (ImGui::BeginViewportSideBar("##SmatchetToolbar", vp, ImGuiDir_Up, h, flags)) {
  // per button: ImGui::SameLine(); draw
}
ImGui::End();
```

`ImGuiDir_Up` reserves a strip below the menu bar and shifts the dockspace down
automatically — no cursor math, no content overlap, identical on standalone (GL) and
Unreal (DX12). Must run **after** `drawMainMenuBar()` and **before** the status bar's
`BeginViewportSideBar(Down)` so the strips carve in order.

Per button: `ImGui::Button(glyph)` sized to bar height; tooltip on hover; disabled style
when (Lua & not built) or (command id unknown). Separator = vertical spacer (consecutive separators collapse to one). Fallback
when `!SmatchetAreFaIconsLoaded()` → first 2 chars of Tooltip/CommandId.

Click → **Command**: `CommandContext ctx{&app, CommandSource::Internal, false, false, 0};`
`app.Commands().Dispatch(btn.CommandId, btn.Args, ctx);` — on `!Ok` toast/log; destructive
commands return confirm-required → confirm popup → re-dispatch with `ConfirmedDestructive=true`.
**Lua**: `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` `app.ExecuteLuaConsoleSnippet(btn.LuaCode,
err, summary)`, toast on fail; `#else` disabled + "Lua automation not built" tooltip.

### Customization UX

- **Right-click button** → Edit…, Move Left, Move Right, Insert Separator, Delete.
- **Right-click empty bar** → Add Button…, Customize Toolbar…, Hide Toolbar.
- **Customize Toolbar dialog** (`SmatchetToolbarEditorUi`):
  - **Scope selector**: `[Global] [Current tracker: <Jira/Plane/GitHub>]`. Global edits the
    shared list; tracker scope edits **only that tracker's append list** (under an
    "Appended for <Tracker>" divider). Read-only preview shows global+append as rendered.
  - **Drag-and-drop reorder** (primary — `BeginDragDropSource` / `AcceptDragDropPayload`,
    confined to the active scope's own list), plus **Up/Down** buttons as keyboard-accessible
    fallback; **Add**, **Duplicate**, **Delete**.
  - **Per-button editor**: Kind; Icon (opens `SmatchetIconPickerUi`); Tooltip; Command → fuzzy
    command picker (`Commands().All()` + `FuzzyScore`, shows id + summary) + Args JSON field
    (validated on commit); Lua → multiline code box. Save / Cancel.
- **Icon picker** (`SmatchetIconPickerUi`): search box (substring over catalog names) + clipped
  glyph grid from the generated catalog; selecting sets both `IconName` and `IconGlyph`.

### Persistence & scope wiring

- **Global** edits → mutate live `TrackerConfig.Toolbar`, then `ConfigManager::Save(config)`.
- **Per-tracker** edits → `LoadPersistentViewsFromDisk()`, key =
  `NormalizeViewsBackendKey(GetTrackerType())`, set `Backends[key].ToolbarAppend`,
  `SavePersistentViewsToDisk()`. Mirrors read-modify-save at `AppController.cpp:1276-1289`.
- **Visibility** persists in `TrackerConfig.Toolbar.Visible` (global; per-tracker visibility deferred).
- Migration: configs without `"toolbar"` load `ToolbarConfig::Default()`; views buckets default
  to empty `ToolbarAppend` (global-only).

### Default buttons (`ToolbarConfig::Default()`, ≤8, all user-editable)

| Button | FA icon | Action | Id |
|---|---|---|---|
| Command palette | `MAGNIFYING_GLASS` | open palette | `ui.command_palette` |
| New view | `PLUS` | open New View dialog | `ui.view_create` |
| Refresh / sync | `ARROWS_ROTATE` | sync active view | resolve concrete id from registry — **see fallback note** |
| Views list | `LIST` | list / manage views | `view.list` |
| *— separator —* | | | |
| Settings | `GEAR` | open settings | `ui.settings` |
| Read-only toggle | `LOCK` | toggle read-only | `app.set_readonly` (`on` = !current) |
| Customize toolbar | `SLIDERS` | open editor | `ui.toolbar_customize` |

> **Sync-button fallback**: `Default()` resolves the sync command id from `Commands().All()` at
> construction. If no global sync command is registered (or ids differ per tracker), **omit the
> Refresh button** rather than bind an unregistered id — consistent with the unknown-command-id
> guard (§ UX Pillar 3). Bind it only once a concrete id is confirmed.

Per-tracker append examples (illustrative — map to each tracker's registered commands; if none,
add one or use a Lua button): **Jira** My Issues `USER`, JQL `MAGNIFYING_GLASS_PLUS`, Boards
`TABLE_COLUMNS`; **Plane** Cycles `ROTATE`, Modules `CUBES`; **GitHub** Issues `CIRCLE_DOT`,
PRs `CODE_PULL_REQUEST`, Branches `CODE_BRANCH`.

## Files to modify

**New**
1. `Source/Core/include/Config/ToolbarConfig.h` — structs + JSON (de)serialize + `Default()`.
2. `Source/Core/src/Config/ToolbarConfig.cpp` — `Default()` + serialize impl.
3. `Source/Core/include/Ui/SmatchetToolbarUi.h` + `src/Ui/SmatchetToolbarUi.cpp` — render bar, click dispatch, right-click menus; owns editor + picker.
4. `Source/Core/include/Ui/SmatchetToolbarEditorUi.h` + `src/Ui/SmatchetToolbarEditorUi.cpp` — Customize dialog (scope, drag-drop reorder, per-button edit).
5. `Source/Core/include/Ui/SmatchetIconPickerUi.h` + `src/Ui/SmatchetIconPickerUi.cpp` — searchable FA glyph grid (`ImGuiListClipper`).
6. `Source/Core/ThirdParty/IconsFontAwesome6/IconsFontAwesome6_Catalog.inl` — generated `{name, glyph}[]`.
7. `scripts/dev/gen-fa-catalog.py` — regenerate the `.inl` from the FA header (run-once / on upgrade).
8. `tests/Source_Core/ToolbarConfig.test.cpp` — round-trip + resolution doctest.

**Modify**
9. `Source/Core/include/Config/ConfigManager.h:61-383,408` — `ToolbarConfig Toolbar;` on `TrackerConfig`; `ToolbarAppend` on `ViewWorkspaceState`; include `ToolbarConfig.h`.
10. `Source/Core/src/Config/ConfigManager.cpp:179,576` — Save `j["toolbar"] = config.Toolbar;`; Load `cfg.Toolbar = j.value("toolbar", ToolbarConfig::Default());`.
11. `Source/Core/src/Config/ConfigManager_Views.cpp:231,269` — (de)serialize `ToolbarAppend`.
12. `Source/Core/include/Ui/SmatchetUI.h:44-135` — `SmatchetToolbarUi toolbar_;` member.
13. `Source/Core/src/Ui/SmatchetUI.cpp:499` — call `toolbar_.Draw(app,d)` after `drawMainMenuBar`, inside `if (!d.cfg.ZenMode)`, before status bar; gated on effective `Visible`.
14. `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp` — View menu: `Show Toolbar` toggle + `Customize Toolbar…`.
15. `Source/Core/src/Commands/Builtin/BuiltinCommands_*.cpp` — register `ui.command_palette`, `ui.settings`, `ui.view_create`, `ui.toolbar_customize`. Toolbar dispatch uses `CommandSource::Internal` in v1; a dedicated `CommandSource::Toolbar` enum (`Command.h:91`) is deferred (§ Out of scope) — not added in v1.

## Existing utilities reused

- `SmatchetUI::Draw()` / `drawMainMenuBar(app,d)` — `Source/Core/src/Ui/SmatchetUI.cpp:499` — insertion point.
- `ImGui::BeginViewportSideBar(... ImGuiDir_Down ...)` — `SmatchetStatusBarUi.cpp:62` — pinned-bar precedent (use `ImGuiDir_Up`).
- `CommandRegistry::Dispatch(name, args, ctx)` — `Source/Core/src/Commands/CommandRegistry.cpp:212` — invoke by id.
- `CommandRegistry::All()` / `FuzzyMatch()` — `CommandRegistry.h:47-54`; `FuzzyScore()` — `FuzzyMatch.h:22` — picker.
- `Command` struct (Name/Summary/Params/Destructive/Handler) — `Command.h:111-136`.
- `AppController::ExecuteLuaConsoleSnippet(code,err,sum)` — `AppController.h:372` / `AppController_LuaBindings_Draw.cpp:900` — Lua run (gated).
- `ConfigManager::Save/Load`, `TrackerConfig` — `ConfigManager.cpp:179`/`:576`, `ConfigManager.h:61-383`.
- `ViewWorkspaceState`, `LoadPersistentViewsFromDisk` / `SavePersistentViewsToDisk` — `ConfigManager.h:408-417,594-595`.
- `GetTrackerBackend()->Connectivity().GetTrackerType()` + `NormalizeViewsBackendKey()` — `AppController.cpp:1255`, `ConfigManager.h:592`.
- `ICON_FA_*` — `Source/Core/ThirdParty/IconsFontAwesome6/IconsFontAwesome6.h`; `SmatchetAreFaIconsLoaded()` — `SmatchetImGuiFonts.h:35`; usage pattern — `SmatchetAiAssistantUi.cpp:119`.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: Toolbar draw is a handful of buttons per frame — trivial. The only non-trivial surface is the icon picker over ~2500 glyphs, mitigated by mandatory `ImGuiListClipper`.
- **Pillar 2 (UI-thread never blocks > 100 ms w/o cue)**: Editor Save triggers sync disk I/O (`ConfigManager::Save` / `SavePersistentViewsToDisk`) on the UI thread, but only on explicit user Save, writing small (≤ few KB) atomic files — same pattern Preferences already uses; no new > 100 ms stall. If profiling ever shows a stall, route the write to a worker.
- **Pillar 3 (never crash)**: RAII throughout; bounds-checked reorder/delete indices; guards for unknown command id, Lua-not-built, and null backend; Args JSON parse wrapped with a logged catch (no empty `catch`). Graceful degradation when FA font absent (text fallback).
- **Pillar 4 (accessibility — aspirational)**: Up/Down keyboard reorder alongside drag-drop; text fallback when glyphs unavailable; inherits ImGui font scaling + theme colors. No auto-fail gate today.

## Perf-review-system gates (diff touches `Source/Core/` → gates apply)

1. **PR-fast CI** — closest scenario: the main-UI / startup-render scenario (toolbar draws every frame in the main viewport). Confirm exact name against `agents/core/perf-gatekeeper.md` § Curated diff → scenario map; add the toolbar-touched UI files to the map if missing. Only the icon-picker path is non-trivial.
2. **Pillar 2 static scanner** — new sync-I/O reachable from `ImGui::*`: the editor Save calls `ConfigManager::Save` / `SavePersistentViewsToDisk` (disk write). User-initiated, small, atomic, matches existing Preferences-save path → no worker required; annotate the call site if the scanner flags it.
3. **Dispatcher drain** — `N/A` — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — `N/A` — no new > 100 ms sync-stall code path (config writes bounded).
5. **Marker inventory** — `N/A` — no `SMATCHET_UI_PERF_SCOPE` markers added (may add one for toolbar draw later; would then regen `docs/perf/MARKER_INVENTORY.md`).

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline against the named scenario before opening the PR.
**Override**: `perf-out-of-band` label only if an intentional regression + baseline-bump PR is queued.

## Risks / non-goals

- **Risk — `ui.*` command additions = scope creep.** Mitigation: tiny, isolated wrappers over existing `d.show*` toggles; v1 could ship with only already-registered commands if needed.
- **Risk — free-text Args JSON invalid.** Mitigation: validate on commit; disable Save until parseable.
- **Risk — icon catalog codegen drift.** Mitigation: `IconGlyph` is the persisted source of truth, so the catalog is a picker convenience, not correctness-critical; regen script checked in.
- **Risk — config Save on UI thread.** See Pillar 2 — bounded + matches existing pattern; accepted.
- **Non-goals**: external-program / URL-open buttons (TC parity; excluded per Command+Lua choice); multiple named button bars (TC `.bar` files); per-tracker visibility; toolbar docking anywhere but under the menu.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `ToolbarConfig` JSON round-trip; effective-resolution (global-only / global+append / empty-append) pure-function tests.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: drive the bar — click a Command button (assert dispatch), open editor, Add → drag-reorder → Delete, open icon picker + select, toggle Show Toolbar; assert toolbar present and dockspace not overlapped.
- **Bash-driver scenario / screenshot / sanitizer**: screenshot with toolbar visible; sanitizer build clean (Pillar 3). Pink-clear gap scan optional (toolbar strip should leave no gap above dockspace).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target). Then exe-staleness `ls -la` on `build/ninja-iter-msvc/Smatchet.exe`.
- **Manual residue**: visual look/feel of the toolbar (icon alignment, spacing) — if no bucket-E visual coverage lands, this hits the § Visual-validation exception (pause + user verdict); deferred-automation action = add a screenshot-diff golden once the bar is stable, logged in `docs/self-improvement/categories/tooling.md`.

## Out of scope (flagged, not designed)

- **External-program / URL buttons** — Total Commander supports them; excluded by the Command+Lua decision. Follow-up plan if requested (add a `Process`/`Url` `ToolbarButtonKind`).
- **Multiple named toolbars / importable `.bar` files** — single bar only for v1.
- **Per-tracker toolbar visibility** — visibility is global-only; per-tracker deferred.
- **Drag-drop across the global↔append boundary** — reorder stays within a scope.
- **`CommandSource::Toolbar` telemetry value** — v1 reuses `CommandSource::Internal`; a dedicated source enum is deferred.

## Implementation log

- `2aa35ea` · Slice 1 — config data model (`ToolbarConfig.h/.cpp`) + persistence (`TrackerConfig.Toolbar`, `ViewWorkspaceState.ToolbarAppend`) + 8 doctest cases.
- `91b08cc` · Slice 2/3 — icon catalog + `SmatchetIconPickerUi`, `SmatchetToolbarUi` (render under menu via `BeginViewportSideBar(Up)`, command/Lua/`ui.*` dispatch, right-click menu, editor with drag-drop reorder + command/icon pickers + `ConfigManager::Save` write-through), `SmatchetUI` integration, View-menu toggle, `SmatchetLuaTests` link fix.
- `d7b1ef2f` (#608) · Slice 4 — replaced the curated ~50-icon catalog with the full Font-Awesome 6 set: `scripts/dev/gen-fa-catalog.py` (empty-parse guard) parses `IconsFontAwesome6.h` and emits the checked-in `IconsFontAwesome6_Catalog.inl` (1402 `{slug, ICON_FA_*}` rows). Catalog + `SmatchetToolbarIconGlyph` lookup moved to a new ImGui-free `SmatchetIconCatalog.cpp` (which `#include`s the `.inl`); `SmatchetIconPickerUi.cpp` renders that shared catalog through `ImGuiListClipper` (UX Pillar 1 — the prior grid had no clipper). New pure test `tests/Core/IconCatalog.test.cpp`.
- `#611` / `#613` · Per-button right-click context menu — `RenderButtonContextMenu` (Edit… / Move Left / Move Right / Insert Separator / Delete) mutates `cfg.Toolbar.Buttons` then `ConfigManager::Save`; menu opened manually (`IsMouseReleased` + `OpenPopup`) so it works on a disabled button too; `realButtonSrc` maps each on-screen non-separator button to its global index; editor preselect via `requestEditSelect_`. Code landed bundled in the #611 scope-selector merge; the non-separator mapping-invariant doctest cases were added separately in #613.

## Deviations from plan

- **Args as `std::string ArgsJson`** (not a live `nlohmann::json` member) — keeps `ConfigManager.h` on `<nlohmann/json_fwd.hpp>`; parsed at dispatch. Also moots the null-json CodeRabbit finding.
- **Test path `tests/Core/`** (plan said `tests/Source_Core/`) — matches actual tree; explicit-listed in `tests/CMakeLists.txt` + `tests/Lua/CMakeLists.txt`.
- **Refresh/sync default button omitted** — no concrete sync command id confirmed (plan § sync-button fallback).
- **`ui.*` as toolbar-layer pseudo-actions** (not registered `BuiltinCommands`) — toolbar intercepts `ui.command_palette` (reuses existing `g_ui.requestCommandPaletteOpen`), `ui.settings` (`cfg.ShowPreferencesWindow`), `ui.toolbar_customize` (own editor). Avoids strict-zone Commands→UI coupling. `ui.view_create` dropped from defaults.
- ~~**Per-button right-click context menu deferred** — only empty-bar right-click (Customize / Hide); edit/move/delete live in the editor. (backlog)~~ — **resolved**: the per-button menu (Edit… / Move Left / Move Right / Insert Separator / Delete) ships, operating on `cfg.Toolbar.Buttons` + `ConfigManager::Save`; opened manually so disabled buttons stay actionable; `realButtonSrc` maps each on-screen button to its global index (appended buttons get no menu). Landed in #611; mapping-invariant tests in #613.
- **Editor per-tracker append scope selector deferred** — editor edits the global toolbar only; per-tracker `ToolbarAppend` still renders (separator-joined) when present in config. (backlog)
- ~~**Curated ~50-icon catalog** instead of generated 2500-glyph `.inl` + clipper — `SmatchetToolbarIconGlyph(name)` lookup; no `gen-fa-catalog.py` shipped. (backlog)~~ — **resolved (Slice 4)**: `gen-fa-catalog.py` ships, `IconsFontAwesome6_Catalog.inl` holds the full FA6 set (1402 glyphs), picker grid clipped with `ImGuiListClipper`.
- **`imgui_internal.h`** required for `BeginViewportSideBar` (matches `SmatchetStatusBarUi.cpp`).
- **Catalog split into `SmatchetIconCatalog.cpp`** (the Slice-4 task phrased it as inlined in `SmatchetIconPickerUi.cpp`) — the table + `SmatchetToolbarIconGlyph` lookup are kept ImGui-free so they're unit-testable: the test-rig refuses ImGui surfaces and the test-delta gate requires coverage for `Source/Core/` changes. The `.inl` remains a single generated source feeding both the lookup and the picker grid.

## Verification (actual)

- **Bucket A**: `SmatchetTests --test-case=*Toolbar*` → 8/8 cases, 30 assertions; `SmatchetLuaTests` → 30/30, 167 assertions (both re-run green on the Slice 2/3 head).
- **Bucket A (context menu)**: `tests/Core/ToolbarConfig.test.cpp` adds two doctest cases locking the mapping invariant `RenderBar`'s per-button menu relies on — resolution preserves the global non-separator subsequence, and global buttons render before the per-tracker append. CI-green, merged via #613.
- **Bucket A (Slice 4, `d7b1ef2f` / #608)**: `tests/Core/IconCatalog.test.cpp` — full catalog populated (>1000 glyphs, every row non-empty), `SmatchetToolbarIconGlyph` lookup (curated-era + new names + empty/unknown), glyph-matches-entry; the full 1402-glyph `.inl` builds dual-target. CI-green via #608.
- **Dual-target build**: `SmatchetStandalone` (OpenGL exe) + `SmatchetCore_DX12` (Unreal lib) compile + link clean (MSVC, isolated worktree).
- **`SmatchetLuaTests` link**: fixed (added `ToolbarConfig.cpp` to its source list — the original PR #603 CI break).
- **Manual / visual**: PASSED — user visually validated the Standalone build (`91b08cc`): toolbar renders below the menu bar, FA icons load, default buttons + hover tooltips, Customize editor (add / drag-drop reorder / icon picker / Save), View-menu Show-Toolbar toggle. **Bucket-E** (ImGui-Test-Engine drive + screenshot golden) still backlogged as deferred-automation.
