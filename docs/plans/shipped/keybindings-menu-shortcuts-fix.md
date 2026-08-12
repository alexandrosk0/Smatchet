<!-- plan-date: 2026-06-15 -->
﻿# Plan — Fix non-working menu-bar shortcuts + "add arbitrary command" button

**Branch:** feat/keybindings-menu-shortcuts (NEW branch off origin/develop). PR #1256 (the
rebindable-shortcut editor, PR2) is ALREADY MERGED into develop, so this is its OWN follow-up PR,
not a slice on the merged PR2 branch.
**Worktree:** C:/Dev/trees/keybindings-menu-shortcuts. All Edit/Write under that prefix.
**Status:** `shipped` — delivered via #1293 (merged 2026-06-16): menu-bar keyboard shortcuts wired to the command registry, the config-load migration fix (`MigrateMenuShortcutKeybindingsV1`), and the Preferences "Add shortcut for a command..." button. See § Implementation log.

## User request (verbatim)
There are many multiple shortcuts displayed in the menu bar that are not working. Go through all of
them, add commands where missing and make the keybindings work properly for them. Zoom In is an
example. In Preferences -> Keyboard Shortcuts tab, add arbitrary new command button.

Three deliverables: (1) audit + fix ALL non-working menu-bar shortcuts; (2) add commands where
missing + make keybindings dispatch; (3) add an "Add shortcut for a command..." button to
Preferences -> Keyboard Shortcuts that binds any registry command with no existing row.

## User decisions locked (do NOT re-ask)
- PR batching: PR #1256 (PR2 editor) is MERGED; ship THIS menu-shortcuts work as its own new PR off
  develop. (Original "add to PR #1256 branch" decision is void -- that PR closed.)
- Conflict resolution: keep Toggle Assistant on Ctrl+Shift+A and Bug Report on Ctrl+Shift+B;
  auto-assign free default combos to the displaced menu items (Clear Selection, Annotate).
  (User approved the INTENT, not exact keys.)
- Conflicts policy = warn-NOT-block; multi-key chords deferred; Zen Ctrl+M then Z stays special-cased.
- Post-ship = "Register with watcher" (authorized auto-merge when green; do NOT self-merge).
- Migrate everything; editor = both (prefs + palette/toolbar).

## Root causes confirmed (prior session, read-only)
- ImGuiHotkey parser cannot parse "=" or "-" -> Zoom In (Ctrl+=) / Zoom Out (Ctrl+-) never bind.
  Ctrl+0 (Reset Zoom) already parses (digit handled).
- Ctrl+Shift+A already = view.assistant; Ctrl+Shift+B already = app.bug_report.open. The menu hints
  "Ctrl+Shift+A" on Clear Selection and "Ctrl+Shift+B" on Annotate are LIES (collide).
- Grid ALREADY self-handles Ctrl+C (focus + WantTextInput-aware) and Esc (clear). dispatchKeybindings
  ignores WantTextInput, so a GLOBAL Ctrl+C/Ctrl+A binding would clobber text-field copy/select-all
  and double-fire. => Keep grid clipboard/selection keys grid-local + focus-scoped (NOT global
  registry commands). Only fix their menu hints.

## Design — two categories

### A. Grid-local (focus-scoped, respects WantTextInput; NOT global/rebindable)
- Copy Ctrl+C -- already works (SmatchetActiveProjectGridUi.cpp ~line 1653).
- Copy Selection Ctrl+Shift+C -- already works (loose effCtrl handler also fires with Shift).
- Clear Selection Esc -- already works. Give it a free global combo too (grid.clear_selection).
- Select All Ctrl+A -- ADD a grid-local handler (see step 3).

### B. App-global rebindable registry commands + Defaults() bindings (safe combos, no text conflict)
New file Source/Core/src/Commands/Builtin/BuiltinCommands_Ui.cpp ->
RegisterUiInteractionCommands(CommandRegistry& reg, AppController& app):
- ui.zoom.in    : g_ui.cfg font size = min(32, +1) + ConfigManager::Save
- ui.zoom.out   : g_ui.cfg font size = max(8, -1)  + Save
- ui.zoom.reset : g_ui.cfg font size = 16          + Save
- ui.open_view  : g_ui.requestCommandPaletteOpen = true; g_ui.requestCommandPaletteFilter = "view.toggle."
- grid.clear_selection : g_ui.focusedPane().gridState.RectSel.ClearAll()
All via RunOnUiThreadAsCommandResult(app, [...]). Category/AsyncSafe mirror ViewToggleCommands.cpp.
Model files: Commands/ViewToggleCommands.cpp, Builtin/BuiltinCommands_BugReport.cpp.
VERIFY exact font-size field name + ConfigManager::Save signature before coding (re-Read; field was
assumed FontSizePt on cfg/TrackerConfig -- confirm).

Wire-up: declare RegisterUiInteractionCommands in Builtin/BuiltinCommands_Internal.h; call it in
BuiltinCommands.cpp::RegisterBuiltinCommands (alongside RegisterViewToggleCommands etc.).
CORE_SOURCES uses GLOB_RECURSE CONFIGURE_DEPENDS -> new .cpp auto-picked, no manual CMake edit.

### Defaults() additions (~10) -- Source/Core/src/Config/KeybindingsConfig.cpp Defaults()
- Ctrl+=        -> ui.zoom.in
- Ctrl+-        -> ui.zoom.out
- Ctrl+0        -> ui.zoom.reset
- Ctrl+Shift+V  -> ui.open_view
- Ctrl+Shift+G  -> grid.clear_selection
- Ctrl+O        -> view.toggle.views_dashboard  args {"action":"show","via":"open_project_view"}
- Ctrl+Shift+E  -> view.toggle.views_dashboard  args {"action":"show"}
- Ctrl+Shift+U  -> view.toggle.log              args {"action":"show"}
- Ctrl+Shift+M  -> view.toggle.backend_audit    args {"action":"show"}
- Ctrl+Shift+N  -> view.toggle.source_annotate  args {"action":"toggle"}  (Annotate, displaced)

### BoundHotkeyDisplayForArgs (semantic args overload)
Needed because Open Project View + Views Dashboard both map to view.toggle.views_dashboard (distinct
args -> distinct keys). Add to KeybindingsConfig.cpp + declare in .h after BoundHotkeyDisplay. Use
nlohmann json semantic == (order-independent). Avoid exceptions: parse with
nlohmann::json::parse(s, nullptr, false) + .is_discarded() (no try/catch -> empty-catch is CRITICAL
in the strict Config zone).

    std::string BoundHotkeyDisplayForArgs(const std::vector<Keybinding>& bindings,
                                          const std::string& commandId, const std::string& argsJson) {
        nlohmann::json want = nlohmann::json::parse(argsJson.empty() ? "{}" : argsJson, nullptr, false);
        if (want.is_discarded()) want = nlohmann::json::object();
        for (const Keybinding& b : bindings) {
            if (b.CommandId != commandId || !b.Enabled || b.Hotkey.empty()) continue;
            nlohmann::json have = nlohmann::json::parse(b.ArgsJson.empty() ? "{}" : b.ArgsJson, nullptr, false);
            if (have.is_discarded()) have = nlohmann::json::object();
            if (have == want) return b.Hotkey;
        }
        return std::string();
    }

### Menu hint rewiring -- Source/Core/src/Ui/SmatchetUI_MainMenu.cpp
Add helper MenuShortcutArgs(ctx, commandId, argsJson, fallback) (mirror MenuShortcut ~line 72 but
calls BoundHotkeyDisplayForArgs). Rewire (line refs approx -- re-Read before editing):
- Open Project View (~253) -> MenuShortcutArgs(views_dashboard, {"action":"show","via":"open_project_view"}, "Ctrl+O")
- Copy (~277) / Select All (~290) / Copy Selection (~296): KEEP literal hints (work grid-local)
- Clear Selection (~293) -> MenuShortcut("grid.clear_selection","Ctrl+Shift+G")
- Zoom In/Out/Reset (~421/425/429) -> MenuShortcut("ui.zoom.in/out/reset",...) + route click to Dispatch
- Open View (~471) -> MenuShortcut("ui.open_view","Ctrl+Shift+V"); keep click inline
- Views Dashboard (~537) -> MenuShortcutArgs(views_dashboard {"action":"show"},"Ctrl+Shift+E"); keep
  click inline (preserves recentViews_.Touch side-effect the command lacks)
- Annotate (~545) -> MenuShortcut("view.toggle.source_annotate","Ctrl+Shift+N")
- Log (~550) -> MenuShortcut("view.toggle.log","Ctrl+Shift+U")
- Backend Audit (~555) -> MenuShortcut("view.toggle.backend_audit","Ctrl+Shift+M")
View-toggle menu CLICKS keep inline (preserve recentViews_.Touch the command lacks); only hints +
zoom clicks route to Dispatch.

### Preferences "Add shortcut for a command..." button -- SmatchetPreferencesUi_Keybindings.cpp
After "Reset all to defaults" (~line 190-194). Reuse app.Commands().All() picker (mirror
SmatchetToolbarUi cmdNames_/cmdSearch_ pattern: InputTextWithHint filter + BeginChild list +
Selectable). On pick: append Keybinding{commandId, "", "{}", true} to d.cfg.Keybindings.Bindings
(exclude commandIds already present as a row); user captures the combo inline in the new row. Then
ui.MarkKeybindingsDirty(); MarkPrefsDirty(d).

## Ordered implementation steps
1. Extend ImGuiHotkey parser: KeyFromToken (ImGuiHotkey.cpp ~23-59) + KeyToToken (~63-86) for
   "="->Equal, "-"->Minus (min) + punctuation/nav-edit set for capture round-trip
   ([ ] backslash ; apostrophe backtick . / Tab Backspace Delete Escape arrows PageUp/Down Home End
   Insert). Update ImGuiHotkey.h:26-30 doc comment. Extend tests/Core/ImGuiHotkey.test.cpp.
2. New BuiltinCommands_Ui.cpp (RegisterUiInteractionCommands) + declare in
   BuiltinCommands_Internal.h + call in BuiltinCommands.cpp. STRICT Commands zone.
3. Grid-local Select All Ctrl+A handler in SmatchetActiveProjectGridUi.cpp
   drawActiveProjectGridRectSelKeys (~1614-1665). Place inside windowFocused but OUTSIDE the
   sel.HasAnySelection() guard (select-all needs no prior selection). Guard:
   windowFocused && !io.WantTextInput && effCtrl && !effShift && IsKeyPressed(ImGuiKey_A,false).
   DRY: extract a shared free helper GridSelectAllRows(GridPane&, const std::vector<CachedTicket>&)
   (declare in include/Ui/SmatchetGridUiSupport.h near CopyGridRectAsTsv/ComputeGridSortSignature)
   and call from both the menu (selectAllGridRows ~228-247) and the grid handler -> avoid dup WARN.
4. KeybindingsConfig: add ~10 Defaults() bindings + BoundHotkeyDisplayForArgs (+ .h decl). Extend
   tests/Core/KeybindingsConfig.test.cpp. STRICT Config zone.
5. Menu hint rewiring (SmatchetUI_MainMenu.cpp): MenuShortcutArgs helper + rewire 13 items; route
   zoom clicks -> Dispatch(ui.zoom.*).
6. Preferences "Add shortcut for a command..." button (SmatchetPreferencesUi_Keybindings.cpp).
7. Locales/*.json: add string keys for the new prefs button + any new labels.

## Verification
- ninja dual-target via scripts/dev/with-msvc-env.sh (MANDATORY wrapper for all cmake/ctest).
- ctest (the two extended unit tests).
- Lint: bash agents/scripts/project/test-lint-rules.sh --diff origin/develop.
- Bucket-E ImGui Test Engine coverage for the new prefs button + a dispatch smoke if feasible.
- Manual: launch exe, verify Zoom In/Out/Reset, Open View, Clear Selection, Select All, and the
  rewired menu hints show the real combos.

## Constraints (carry forward)
- All Edit/Write under C:/Dev/trees/keybindings-menu-shortcuts prefix.
- Commit via literal: git -C /c/Dev/trees/keybindings-menu-shortcuts (guard blocks variable-path).
- Windows text plumbing (NOT MSYS-specific -- it is the Bash-tool wrapper's command marshalling, not
  MSYS bash, which handles heredocs fine). PowerShell is the primary shell; reserve the Bash tool for
  the project's own .sh tooling (test-lint-rules.sh, with-msvc-env.sh, merge-gates.sh, bats):
  * Commit/PR bodies: Bash heredoc piped to `git commit -F -` / `gh ... --body-file -` (PS here-string
    injects a stray @). The heredoc must be the SOLE command in the Bash call (cd/echo around it
    breaks the wrapper's heredoc recognition); or write a tmpfile via PowerShell + `commit -F <tmpfile>`.
  * FILE writes: the Bash tool's wrapper mangles multi-line heredocs on Windows -- use the PowerShell
    tool: single-quoted here-string + [IO.File]::WriteAllText($p,$c,(New-Object System.Text.UTF8Encoding $false))
    (UTF-8 no-BOM; `Set-Content -Encoding utf8` in PS 5.1 adds a BOM + mis-reads existing UTF-8 on round-trip).
  * Builds go through MSVC (scripts/dev/with-msvc-env.sh), not MSYS gcc (MSYS2 retired as a build
    toolchain). Real MSYS gotcha = path-mangling: MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' ONLY for
    colon-path `git show "ref:path"`, never for `git/gh -C`.
- Do NOT self-merge (merge-watcher is the authorized agent for green PRs); never merge past ANY red
  check; never trust a CodeRabbit "Addressed" annotation blindly.
- Strict lint zones {Tracker,Sync,Persistence,Config,Commands}+Mcp. KeybindingsConfig.cpp +
  BuiltinCommands_Ui.cpp = STRICT. ImGuiHotkey.cpp + SmatchetUI*.cpp + prefs UI = LIGHT/ungated.
- C++14 hard; LOG_* only; RAII no raw new/delete; MSVC+Clang; warnings-as-errors; nlohmann
  obj["k"]=v not brace-list; function-size 120 non-UI / 200 ImGui-draw; branchiness <=30.
- Commit trailer: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
- PR body trailer: Generated with Claude Code (https://claude.com/claude-code).

## Why the relaunch happened
Prior session anchored to the shared integration tree (C:/Dev/Smatchet, codex/tooltip-wheel-router).
A sibling moved its HEAD; guard-head-drift.sh (path-blind for Edit/Write) blocked all edits. When
PR #1256 then merged, the old feat/keybindings-editor-pr2 branch went post-squash-stale, so this
work was re-based onto a fresh feat/keybindings-menu-shortcuts branch off origin/develop.
Relaunch with cwd = this worktree so the guard evaluates the stable feat/keybindings-menu-shortcuts
HEAD (drift-proof, Workflow-capable).

## Implementation log
All 7 ordered steps landed across two commits on feat/keybindings-menu-shortcuts:
- `cf0f06c7` wip(plan): handoff doc.
- `51f77d74` feat(ui): wire menu-bar keyboard shortcuts to the command registry — steps 1-7
  (ImGuiHotkey parser "=" / "-" + punctuation/nav set; BuiltinCommands_Ui.cpp
  RegisterUiInteractionCommands; grid-local Select All Ctrl+A + shared GridSelectAllRows helper;
  ~10 Defaults() bindings + BoundHotkeyDisplayForArgs; MenuShortcutArgs rewiring of 13 menu items +
  zoom-click Dispatch; Preferences "Add shortcut for a command..." button; Locales string keys).
- `806e0ba8` merge origin/develop (fast-forward of upstream; no conflicts in touched files).
- Follow-up (this session, uncommitted at log time → committed next): the migration fix + ImGuiHotkey
  table refactor + new tests (see Deviations).

## Deviations
1. **NEW root cause found + fixed — config load REPLACES keybindings (the real reason Zoom In stayed
   dead for upgrading users).** `KeybindingsConfig::from_json` clears then loads only the JSON-present
   bindings — it never merges in defaults. So an existing `%LOCALAPPDATA%\Smatchet` config (every
   upgrading user) never receives the 9 new menu-shortcut default bindings; the parser fix alone
   (step 1) was necessary but not sufficient. Fix: one-shot idempotent migration
   `MigrateMenuShortcutKeybindingsV1(j, cfg)` in ConfigManager.cpp (STRICT Config zone), gated on a
   persisted `migrated_menu_shortcuts_v1` flag (`TrackerConfig::MigratedMenuShortcutsV1`, saved in
   ConfigManager.h). It seeds only the new command ids (ui.zoom.in/out/reset, ui.open_view,
   grid.clear_selection, view.toggle.{views_dashboard,log,backend_audit,source_annotate}) and only
   when that exact `(CommandId, ArgsJson)` is absent (`FindBindingIndex < 0`) — a user's existing
   rebind of the same command is never duplicated or overwritten. Called immediately after
   `MigrateBugReportHotkeyToKeybindings` in `LoadListFields`. No try/catch (strict-zone empty-catch is
   CRITICAL): reads via `j.value(...)`. Covered by 4 new TEST_CASEs in ConfigMigration.test.cpp
   (seed-into-existing, respect-user-rebind-no-dup, flag-on-disk-skips-seed, both-views_dashboard-arg-
   variants + idempotent-on-reload) — 26 assertions, all pass.
2. **ImGuiHotkey.cpp KeyFromToken/KeyToToken refactor (LIGHT zone).** Replaced the two long switch
   statements with a single `kNamedKeys[]` table (`{token, ImGuiKey, canonical}`) to keep
   KeyFromToken under the branchiness cap after adding the punctuation/nav tokens; letters/digits/
   F-keys stay offset arithmetic. Net −122 lines.
3. **2 corrected comments in keybindings_editor_rebind.test.cpp** — MarkKeybindingsDirty is public but
   no accessor exposes the live SmatchetUI instance; Test C now credits BOTH root causes (parse fix +
   config-merge migration).
4. **Tooling deviations (process, not product):** DX12 configure run via the PowerShell tool, not the
   Bash tool — the Bash tool's MSYS bash mangles `/DWIN32` → `C:/Program Files/Git/DWIN32` on a fresh
   configure (C1083). PS-5.1 `2>&1` on a native exe wraps stderr as NativeCommandError and falsifies
   `$LASTEXITCODE`/`$?` even on success — read the real code via a trailing `"EXITCODE=$LASTEXITCODE"`
   instead of redirecting. ctest run via `--test-dir build/ninja-test-msvc` (no testPresets defined).

## Verification results
- **Dual-target build** — standalone `Smatchet.exe` (ninja-iter): clean (`ninja: no work to do` on
  no-op rebuild, fresh exe). Unreal DX12 `SmatchetCore_DX12.lib` (ninja-iter-unreal-msvc): configured
  via PowerShell (CMAKE_CXX_FLAGS `/DWIN32 /D_WINDOWS /EHsc`, no MSYS mangling), built 503/504,
  lib linked, EXITCODE=0.
- **ctest** — `ctest --test-dir build/ninja-test-msvc --output-on-failure`: 7/7 passed (16.59s).
  The 4 new menu-shortcut migration cases run green directly (`--test-case="*menu shortcuts*"`):
  4/4, 26 assertions, 0 failed.
- **Lint** — `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop`: PASS on every
  gated rule (strict-zone, comment-noise, no-raw-new, oversize, include-cycle, agent-size). Only
  WARN: comment-ratio on ConfigManager.h (52%) + KeybindingsConfig.h (57%) — advisory, non-blocking
  (header doc comments).
- **Bucket-E (ImGui Test Engine)** — `Smatchet.exe cmd ui_test.run --name=Keybindings --spawn`:
  3/3 pass (`passed:3, tested:3, failed:0, ok:true`) — EditorTabRendersWithLiveConflict,
  DefaultComboDispatchesToCommand, ZoomComboAdjustsFontSize (line 208 `'grew'` OK — was the failing
  assertion before the migration; the migration is the fix). The spawned child exits 4 with a
  `std::terminate` on teardown; proven pre-existing harness noise by running an untouched suite
  (DurationInlineEdit) through the same `--spawn` path — identical exit-4/std::terminate after all its
  tests report `Success.`. Bucket-C/E are not required CI checks (dropped 2026-06-15, Mesa-GL lanes
  can't boot the CI exe), so this does not gate merge.
- **Manual** — surfaced as a post-ship "Manual verify" option (launch exe; confirm Zoom In/Out/Reset,
  Open View, Clear Selection, Select All, and the rewired menu hints show the real combos).
