# Plan — Input UX Wave 2, piece 4.6: keyboard equivalents for pointer gestures
<!-- plan-date: 2026-11-20 -->

> **Slug**: `input-ux-w2-gesture-keys` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — piece 4.6 of [`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 4.6, PR #2440 (squash `79baaa7a4`). The deferred half (§ Out of scope) is tracked in `docs/self-improvement/categories/debt/2026-11-22-gesture-keys-deferred.md`.
>
> **Usage**: authored with the `author-plan-doc` skill from `_plan-template.md`. The audit's § 3 keyboard contract gains the rule this plan enforces.

## Context

The input UX audit (§ 4.6) found that reorder, resize, calendar picking, sort editing and several list actions exist only as mouse gestures. It assigns findings #12, #18, #29, #30, #44, #71, #108, #119, #122, #130 and #138 to this piece. Wave 1 (#2433 and develop's #2430) has already closed some of them:

- **#108 is closed.** Enter on a focused conflict row opens the resolver (`SmatchetOfflineQueueUi.cpp`, `PlainEnterPressed`).
- **#18 is half closed.** The palette has a Cancel button (#2430). The outside-click close is still open, but `CommandPaletteUi.cpp` is under the Wave 1 lock (`input-ux-wave1-palette`), so it is deferred (§ Out of scope).

Three sibling locks overlap the rest:

- `shared-dropdown-recents` (#2432) holds the grid header sort popup, the views-dashboard widgets and every value picker. #2432's `FilterDropdown` gives those pickers Up/Down/PageUp/PageDown/Enter.
- `bulk-edit-rows` (#2434) holds the grid table and `SpreadsheetState.h`.
- `input-ux-wave1-palette` (#2433) holds the palette.

The user chose (2026-11-16) to ship the unlocked half now and defer the rest. The deferred half is the grid column commands, the sort popup, the views-dashboard reorder rebind, the palette outside click, and the popup lists that should reuse `FilterDropdown`. The append-only shared files are the one exception: `SmatchetLocalization.cpp`, `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp`. This PR crosses those locks under `plan-lock-out-of-band`, as the three siblings do.

**Outcome:** every drag gesture in an unlocked file has a registered command with a default, rebindable key. A check script lists any drag source that has neither a command nor a recorded exemption, and the keyboard contract carries the rule.

## Approach

**One focus-scoped latch for widget-local gestures.** A drag that moves a row or a splitter belongs to one widget, and only the focused widget should act on the key. Four new commands post a one-shot request into a small UI-thread latch (`Ui/GestureKeys.h`): `ui.list.move_up`, `ui.list.move_down`, `ui.list.remove` and `ui.splitter.grow` / `ui.splitter.shrink`. Each widget that owns such a gesture takes the request only while its window has keyboard focus.

Posting and taking are frame-stamped, and a request lives for the frame it was posted in plus the next one. A keybinding fires inside `SmatchetUI::Draw`, and a request from MCP or Lua lands in `MainThreadDispatcher::Drain` at the top of `Draw`, so either way the widgets drawn later that frame see it. The one-frame grace covers a widget drawn before the dispatch in frame order, such as a test harness's `GuiFunc`. A request nobody takes expires, so a key pressed with no list focused does nothing.

**Panels are named, not focus-scoped.** `view.panel.grow` / `view.panel.shrink` take `panel` = `focused` (the default), `bottom`, `primary` or `secondary`. `focused` resolves to the edge panel whose dock node holds the focused window. The nudge writes the panel's pending extent through the same deferred path a reveal drag uses (`EdgePanelDragState::pendingExtent` + `applyFrames`, applied by `ApplyPendingExtent` in `Tick`), so it is clamped and persisted exactly like a drag.

**Widget keyboard flows that need no command:**

- **Calendar (#44).** The popup opens with the keyboard on the selected day. PageUp / PageDown move a month, Shift+PageUp / Shift+PageDown a year, Home / End go to the first / last day, T goes to today, and Ctrl+Enter applies in the full picker. A row of quick picks (Today, Tomorrow, In a week) sits above the grid. The arrows keep ImGui's spatial navigation between day buttons, and Enter picks the focused day. While the popup is focused it claims PageUp / PageDown / Home / End with `SetKeyOwner` every frame, so ImGui's own page navigation does not move the cursor underneath.
- **Path remaps (#130).** Add path remap puts the caret in the new row's *from*. Enter in a row's *to* adds the next row when that row is the last and filled.
- **Read-only text (#12).** Ctrl+A works while the run has keyboard focus (a click gives it focus, or Tab reaches its hit box), not only while hovered, and the focused run draws the nav focus rectangle.

**Toolbar editor list (#122).** The ##tblist child takes the list requests: Alt+Up / Alt+Down move the selected button, and Delete removes it. These are the same operations as the Up / Down / Delete buttons, factored into members. They act on the *selected* row (Space or Enter selects the row under the keyboard cursor), and after a move the keyboard cursor follows the moved row. Selection does not follow the cursor: ImGui's own arrow navigation also fires on Alt+Up / Alt+Down, so a follow-focus rule would make Alt+Down move the row the cursor just stepped onto (see § Deviations).

**Check script.** `scripts/dev/test-drag-keyboard-equivalent.sh` scans first-party C++ under `Source/` for drag sources:

- `BeginDragDropSource(`
- `IsMouseDragging(`
- `GetMouseDragDelta(`
- `ImGuiTableFlags_Reorderable`

Each match needs one of the following within the three lines above it or on its own line:

- a `keyboard-equivalent: <command.id>[, <command.id>…]` comment whose ids are all registered (`Name = "<id>"` or `MakeCommand("<id>"` under `Source/Core/src/Commands/`) and bound in `KeybindingsConfig::Defaults()`;
- a `keyboard-equivalent: native(<why>)` comment, for a gesture whose keyboard path is ImGui's own;
- a `SMATCHET_DEVIATION(rule=drag-without-command; …)` marker.

The script runs with `--check` (the default) or `--selftest`. Fixture trees under a temp root assert each violation class fails and each clean form passes. It is wired like `test-no-new-ps1.sh`: `test-all.sh` enrols it by name, so `agentic-selftests` runs it in CI, and a bats wrapper covers it. A site in a locked file (the views-dashboard handle, the grid table, `SmatchetGridColumnInteraction.cpp`) carries the deviation marker in this PR only if its file is unlocked. Otherwise it is listed in the script's pending table with the follow-up it waits on, and printed as a WARN.

## Interface contracts

New shared headers (the reason this plan exists, `Source/Core/src/Ui/AGENTS.md` § Before you edit):

- **`Source/Core/include/Ui/GestureKeysPure.h`** (header-only, no ImGui):

  ```cpp
  namespace smatchet { namespace ui {
  enum class GestureKey : int { ListMoveUp = 0, ListMoveDown, ListRemove, SplitterGrow, SplitterShrink, Count };
  class GestureKeyLatch {
  public:
      void Post(GestureKey key, int frame);
      bool Take(GestureKey key, int frame);   // true once, when posted at `frame` or `frame - 1`
      void Clear();
  };
  bool IsFocusScopedGestureCommand(const std::string& commandId); // "ui.list." / "ui.splitter." prefix
  }}
  ```

- **`Source/Core/include/Ui/GestureKeys.h`** + `Source/Core/src/Ui/GestureKeys.cpp` (UI thread only):

  ```cpp
  namespace smatchet { namespace ui {
  void PostGestureKey(GestureKey key);                        // stamps ImGui::GetFrameCount()
  bool TakeGestureKey(GestureKey key, bool targetHasFocus);   // leaves the request when !targetHasFocus
  bool TakeGestureKeyForCurrentWindow(GestureKey key);        // focus = IsWindowFocused(RootAndChildWindows)
  }}
  ```

- **`SmatchetBottomPanelDrag.h`** gains a target enum and two functions:

  ```cpp
  enum class PanelTarget { Focused, Bottom, Primary, Secondary };
  struct PanelNudgeResult { bool Ok = false; const char* Panel = ""; float ExtentPx = 0.0f; const char* Error = ""; bool NoFocusedPanel = false; };
  PanelNudgeResult NudgePanelExtent(UiDrawSession& d, PanelTarget target, float deltaPx);
  ```

- **Commands** (`Source/Core/src/Commands/GestureKeyCommands.cpp`, registered from `BuiltinCommands.cpp` right after `RegisterAppViewCommands`). Every command returns its envelope from `RunOnUiThreadAsCommandResult`.

  | Command | Args | Default keys |
  |---|---|---|
  | `ui.list.move_up` | — | `Alt+Up` |
  | `ui.list.move_down` | — | `Alt+Down` |
  | `ui.list.remove` | — | `Delete` (bare key: skipped while a text field has the caret) |
  | `ui.splitter.grow` | — | `Shift+Alt+Down`, `Shift+Alt+Right` |
  | `ui.splitter.shrink` | — | `Shift+Alt+Up`, `Shift+Alt+Left` |
  | `view.panel.grow` | `panel` (enum, default `focused`) | `Ctrl+Alt+PageUp` |
  | `view.panel.shrink` | `panel` (enum, default `focused`) | `Ctrl+Alt+PageDown` |

  The `ui.list.*` / `ui.splitter.*` handlers return `{"posted": true}`. The panel handlers return `{"resized": true, "panel": "<bottom|primary|secondary>", "extent": <px>}`. With `panel=focused` and no panel holding the focus (the usual case when the keyboard is in the grid) they return `{"resized": false, "reason": ...}` as a success, so a key press and its repeats log nothing. A named panel that is hidden, or has no resizable edge, is a `ValidationError`.

- **Config:** `TrackerConfig::MigratedGestureKeysV1` (`migrated_gesture_keys_v1`) seeds the new defaults once into an existing config. This uses the same skip-if-the-identity-is-bound contract as `MigrateMenuShortcutKeybindingsV1`.
- **Contract for piece 4.1 (dialog scope, built on an unpushed branch):** 4.1 mutes global shortcuts while a dialog is open. The focus-scoped gesture commands must stay live under that mute, because they act only on the focused widget inside the dialog (the Customize Toolbar modal, the long-text editor). 4.1 consults `IsFocusScopedGestureCommand(id)` before muting.

## Files to modify

**Shared / new**

1. `Source/Core/include/Ui/GestureKeysPure.h` (new): the latch + predicate.
2. `Source/Core/include/Ui/GestureKeys.h`, `Source/Core/src/Ui/GestureKeys.cpp` (new): the UI-thread instance.
3. `Source/Core/include/Commands/GestureKeyCommands.h`, `Source/Core/src/Commands/GestureKeyCommands.cpp` (new): the seven commands.
4. `Source/Core/src/Commands/BuiltinCommands.cpp`: one `RegisterGestureKeyCommands` call.

**Config**

5. `Source/Core/src/Config/KeybindingsConfig.cpp`: the defaults.
6. `Source/Core/include/Config/ConfigManager.h`, `Source/Core/src/Config/ConfigManager_Load.cpp`, `Source/Core/src/Config/ConfigManager_Save.cpp`: the V1 seed flag.

**Consumers**

7. `Source/Core/src/Ui/SmatchetToolbarUi.cpp`, `Source/Core/include/Ui/SmatchetToolbarUi.h`: the list keys, the cursor following a moved row, and factored move/remove (#122).
8. `Source/Core/src/TicketFieldEditor_Modal.cpp`: the long-text split splitter.
9. `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp`: the script/log splitter.
10. `Source/Core/src/Ui/SmatchetMcpServerUi.cpp`: the info-panel splitter.
11. `Source/Core/include/Ui/SmatchetBottomPanelDrag.h`, `Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp`: the panel nudge (#138, panels).
12. `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp`: the calendar keys and quick picks (#44).
13. `Source/Core/include/Tracker/TrackerDateTimePure.h`, `Source/Core/src/Tracker/TrackerDateTimePure.cpp`: `AddDaysToDate`, `AddMonthsClampDay`, `TodayLocalDate`.
14. `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp`: the path-remap focus and Enter flow (#130), plus a `SMATCHET_BUILD_UI_TESTS` draw seam.
15. `Source/Core/src/Ui/SelectableTextRun.cpp`: focus-gated Ctrl+A and the focus rectangle (#12).
16. `Source/Core/include/TouchCellEditGesture.h`: a `keyboard-equivalent: native(…)` marker (touch scroll discrimination).

**Check, docs, l10n**

17. `scripts/dev/test-drag-keyboard-equivalent.sh` (new), `scripts/dev/test-drag-keyboard-equivalent-bats.sh` (new), `tests/bats/drag_keyboard_equivalent.bats` (new).
18. `docs/audits/INPUT_UX_AUDIT.md` § 3: the gesture rule. `docs/guides/keyboard-shortcuts.md`: the new defaults and the widget keys.
19. `Source/Core/src/SmatchetLocalization.cpp`: French rows for the new strings (lock crossed out of band).

**Tests**

20. `tests/Core/GestureKeysPure.test.cpp` (new), `tests/Core/TrackerDateTimePure.test.cpp`, `tests/Core/KeybindingsConfig.test.cpp`, plus the migration test file; `tests/CMakeLists.txt` lists the new test (lock crossed out of band).
21. `tests/ui/gesture_keys.test.cpp` (new), `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp` (lock crossed out of band).

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Commands/BuiltinCommands.cpp` | `RegisterAppViewCommands` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp` | `ApplyPendingExtent` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetToolbarUi.cpp` | `DrawEditorButtonList` | `CORE_SOURCES` |
| `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp` | `DrawCalendarPicker` | `CORE_SOURCES` |
| `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp` | `DrawAnnotatePathRemaps` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SelectableTextRun.cpp` | `HandleSelectionKeyboardAndMenu` | `CORE_SOURCES` |
| `Source/Core/src/Config/ConfigManager_Load.cpp` | `MigrateQuickCreateKeybindingV1` | `CORE_SOURCES` |

## Existing utilities reused

- `RunOnUiThreadAsCommandResult` (`Source/Core/include/Commands/MainThreadDispatch.h:101`): every handler, which runs inline on the UI thread.
- `ApplyPendingExtent` / `EdgePanelDragState` (`SmatchetBottomPanelDrag.cpp:507`): the panel nudge reuses the reveal's deferred, clamped extent write.
- `WindowInsideNode` / `EdgePanelNode` / `NodeShowsContent` (`SmatchetBottomPanelDrag.cpp`): resolving `focused`.
- `MigrateQuickCreateKeybindingV1` (`ConfigManager_Load.cpp`): the seed-once migration shape.
- `MatchHotkey` + `SmatchetUI::dispatchKeybindings`: unchanged. The new commands are ordinary bindings.
- `HandleRowReorder` (`SmatchetViewsDashboardUi_widgets.cpp:120`): the in-tree precedent for Alt+Up / Alt+Down reorder. Its rebind waits on #2432's lock.
- `TrackerDateTimePure::DaysInMonth` / `IncMonth` / `DecMonth` / `ClampDayToMonth`: the calendar date math.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted. The toolbar editor gains two small members (move / remove) that its buttons and the list keys share.

## UX Pillar callouts

- **Pillar 1 (perf, 6.94 ms):** the latch is an array of five ints. Each consumer adds one focus check per frame and no allocation.
- **Pillar 2 (never freezes):** no I/O. The panel nudge writes dock `SizeRef`s, and saving the layout stays on the existing ini path.
- **Pillar 3 (never crash):** every index is bounds-checked before a move or remove, the panel nudge checks node, parent and children for null, and the date math clamps to valid days.
- **Pillar 4 (accessibility):** this is the point of the plan. Every gesture in scope gets a keyboard path, and focused runs and calendar days show the nav cursor.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI:** fires on the standard set. No per-frame work is added beyond a focus check in the consumers, so no scenario targets this path.
2. **Pillar 2 static scanner:** N/A. No sync I/O is added.
3. **Dispatcher drain:** N/A. `Drain` is untouched, and a command from MCP or Lua posts its request from inside the existing drain.
4. **Visible-cue bucket-E harness:** N/A. No new stall path.
5. **Marker inventory:** N/A. No new `SMATCHET_UI_PERF_SCOPE` markers.

## Risks / non-goals

- **Key conflicts.**
  - `Alt+Up` / `Alt+Down` and `Shift+Alt+arrows` were unbound.
  - `Delete` is a bare key, which `dispatchKeybindings` already skips while a text field has the caret.
  - `Ctrl+Alt+PageUp` / `PageDown` type nothing under AltGr.
  - The `KeybindingsConfig` doctest pins the defaults.
- **4.1 mute.** If 4.1 mutes every global binding inside a dialog, the toolbar editor and long-text splitter keys go dead. This is mitigated by the § Interface contracts entry and the predicate, and the PR body names it for 4.1.
- **Stale request.** A key pressed with no target focused could act a frame later on a widget that gains focus. This is bounded by the one-frame lifetime, and a focus change needs a click or a key, which takes at least a frame.
- **Save-format.** One additive config flag, defaulting to false. No view, cache or MCP wire changes beyond the new commands' schemas.
- **Dual target.** `GestureKeys.cpp` and the commands compile into `SmatchetCore_DX12`. The new headers include no GLFW or OpenGL, and `LuaConsolePlugin` builds only with Lua on.
- **Non-goals.**
  - Undo for a keyboard Delete (piece 4.3 owns undo; Delete does exactly what the button does).
  - Typing exact times into the calendar's three drag fields (ImGui's own Enter-to-type covers it).
  - Column resize on read-only tables (`ImGuiTableFlags_Resizable` outside the grid), which is out of the check's scope; see § Out of scope.

## Implementation handoff

Single slice, orchestrator; Ui subsystem with Commands / Config / Tracker touch points.

- **Write set:** § Files to modify. Everything else is out of scope.
- **Pre-resolved decisions:**
  - Commands stay plain English (`Summary` / `Params`). User-facing labels go through `SmatchetLocalization::T` with a French row.
  - A command never mutates widget state directly; it posts or names a target, and the widget applies it in its own draw.
  - Strict zones (Commands, Config, Tracker) get no `printf`, no raw `new`, and `LOG_*` only.
- **Order:**
  1. The latch + commands + defaults + migration.
  2. The consumers.
  3. The check script, which needs the markers.
  4. Docs, then tests.

## Verification

- **Bucket A (doctest):**
  - `GestureKeysPure.test.cpp`: post / take / expiry / double-take / per-key independence, and the predicate.
  - `TrackerDateTimePure.test.cpp`: day and month arithmetic across month and year ends and leap years.
  - `KeybindingsConfig.test.cpp`: the new defaults.
  - Migration: seeds once, keeps a user binding, and never resurrects a cleared row.
- **Bucket A (bash):** `bash scripts/dev/test-drag-keyboard-equivalent.sh` passes on the tree, `--selftest` passes, and the bats wrapper passes.
- **Bucket E (`ninja-ui-test-msvc`, `tests/ui/gesture_keys.test.cpp`).** Each command is driven by its default key through the live `dispatchKeybindings`:
  - `Alt+Up` / `Alt+Down` / `Delete` on a hosted toolbar editor list;
  - `Shift+Alt+Down` / `Up` on the MCP info splitter;
  - `Ctrl+Alt+PageUp` / `PageDown` on the bottom panel;
  - the calendar keys (T, PageDown, Shift+PageDown, Home, End, Enter, Ctrl+Enter) on a hosted `RenderGenericDatePicker`;
  - the remap Add focus and Enter-adds-next flow;
  - Ctrl+A on a keyboard-focused `SelectableTextRun` with the mouse elsewhere.

  Each test is mutation-checked: the consumer's take or key branch is reverted on a scratch commit, CI shows the test red, and the scratch commit is reverted. The results go in § Verification (actual).
- **Build gate:** `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`, on Windows CI (this container has no MSVC). Locally, `posix-core-check` compiles the Core TUs with clang.
- **Doc validation (blocks plan-doc PRs — keep this bullet):** the canonical `scripts/dev/test-docs.sh` suite passes.
- **Plan stress-test — `grill-with-docs` (keep this bullet):** stress-test the plan's terms against the audit's § 3 contract and the Ui / Commands leaf docs before finalising, and record the outcome in § Verification (actual).
- **Manual residue:** none planned. Anything left manual gets a `docs/self-improvement/categories/tooling/` entry.

## Open questions

- None. Slice 1 of piece 4.1 (#2437) mutes app-wide shortcuts under a modal through `dialogkeys::GlobalShortcutAllowedUnderModal`; this piece extends that predicate with `IsFocusScopedGestureCommand`, so `ui.list.*` and `ui.splitter.*` keep working in the Customize Toolbar editor and the long-text editor (`DialogKeysPure.h`, covered by `SmatchetWorklogSubmitPure.test.cpp`). The panel commands stay muted under a modal.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)**: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md` and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

Deferred, now tracked in `docs/self-improvement/categories/debt/2026-11-22-gesture-keys-deferred.md` (#2432 and #2434 have merged, so the locks that held these are gone):

- **#138 columns:** `grid.column.move_left` / `move_right` / `narrow` / `widen` on the focused pane's current column. These touch `SmatchetActiveProjectGridTable.cpp` and `SpreadsheetState.h`, which `bulk-edit-rows` holds.
- **#71:** a filterable Add Sort Rule list, plus `ui.list.*` on the sort rules. These touch `SmatchetGridHeaderUi.cpp`, which `shared-dropdown-recents` holds. The list reuses `FilterDropdown`.
- **The views-dashboard row reorder:** rebinding its hardcoded Alt+Up / Alt+Down to `ui.list.*` touches `SmatchetViewsDashboardUi_widgets.cpp`, which `shared-dropdown-recents` holds.
- **#119 and the other filter + list popups:** the toolbar command picker, the Keybindings *Add shortcut* popup and the icon picker move onto #2432's `FilterDropdown` (the user's 2026-11-16 decision: no second list-navigation helper).
- **#18 remainder (outside click closes the palette):** `CommandPaletteUi.cpp` is held by `input-ux-wave1-palette`.
- **#29 / #30 (active cell, cell ranges):** Wave 3, grid as a spreadsheet. The rect-select drag gets its keyboard path (Shift+arrows) there.
- **Column resize on read-only tables** (`ImGuiTableFlags_Resizable` in the audit, perf, offline-queue, annotate and bulk tables): needs a table-generic resize command. Backlog entry `docs/self-improvement/categories/debt/2026-11-16-readonly-table-column-resize-keys.md`.
- **Undo for a keyboard remove:** piece 4.3.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

- `5cbe4fc` · wip(plan): this plan, committed before any code.
- `b6f782a` · merge `origin/develop` (Wave 1 #2433 and piece 4.1 slice 1 #2437) into the branch; the squash-merge conflicts resolved to develop's side, and the result equalled develop plus this plan.
- `6dc3281` · the piece itself:
  - the focus-scoped latch (`Ui/GestureKeysPure.h`, `Ui/GestureKeys.h`) and the seven commands (`Commands/GestureKeyCommands.cpp`);
  - default bindings and the `migrated_gesture_keys_v1` seed;
  - consumers: the Customize Toolbar list, the long-text, MCP and Lua dividers, the panel nudge, the calendar keys and quick picks, the path-remap flow, and read-only text select-all;
  - `scripts/dev/test-drag-keyboard-equivalent.sh` with its selftest and bats suite;
  - the rule in the audit's § 3 and the shortcuts guide, French strings, and the doctest and bucket-E tests.
  - `GlobalShortcutAllowedUnderModal` (`DialogKeysPure.h`, from #2437) now lets `ui.list.*` and `ui.splitter.*` through, so they work inside the dialogs they serve.
- `79baaa7a4` · squash-merge of PR #2440 into `develop`.

Findings: **#108** was already closed by Wave 1. **#18** is half closed (the Cancel button, #2430); the outside click is deferred. **#12, #44, #130, #122** and the panel half of **#138** are closed here. **#71, #119** and the column half of **#138** are deferred (below). **#29, #30** stay with Wave 3.

## Deviations from plan

- **Selection does not follow keyboard focus in the toolbar list** (the plan said it would). ImGui's own arrow navigation also fires on Alt+Up / Alt+Down (`NavUpdateCreateMoveRequest` reads the arrows with no modifier test), so a follow-focus rule would reset the selection to the row the cursor just stepped onto, and Alt+Down would move the wrong button, worse under key repeat. The keys act on the *selected* row (Space or Enter selects), as the Up / Down / Delete buttons do, and the moved row takes the cursor back through `SetKeyboardFocusHere`. Residual: arrow to a row and press Delete, and the previously selected row goes. Accepted by the review.
- **The panel commands do not fail when `panel=focused` finds no panel.** They return `{"resized": false, "reason": ...}` as a success, because that is the normal case with the keyboard in the grid, and a failure would log a warning on every press and key repeat. A *named* panel that is hidden is still a `ValidationError`.
- **Toolbar tooltips name the command, not the key** (`ui.list.move_up`, ...), since the bindings are rebindable.
- **Open question closed:** piece 4.1 slice 1 (#2437) did mute app-wide shortcuts under a modal; this piece extends `GlobalShortcutAllowedUnderModal` rather than adding a second mechanism.
- **Plan locks.** The `refs/locks/input-ux-w2-gesture-keys` claim could not be filed: pushes to `refs/locks/*` returned HTTP 403 from the session's git proxy, with the claim script's own retries and a later retry both failing. The four append-only shared files (`SmatchetLocalization.cpp`, `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`) are held by sibling locks, so the PR carries `plan-lock-out-of-band` for them, as #2432, #2433 and #2434 do. No feature file under a sibling lock was touched.
- **First bucket-E run (6 of 8 passed).** The two failures were test-side and are recorded here with their causes:
  - `ToolbarList_KeysMoveAndRemove`: the test engine delivers Alt and the arrow in one frame, and ImGui's Alt-release menu-layer toggle (`NavUpdateWindowing`) then moves focus off the list on the next frame, so the key request found the list unfocused. A person holds Alt before the arrow, which cancels the toggle, so the production gate stays the strict this-frame `IsWindowFocused()` (a previous-frame flag is not allowed to guard a mutation). The case now presses Alt first. Residual: a chord whose Alt and arrow land in the same frame can lose to the toggle.
  - `PanelResize_KeysNudgeBottom`: the bottom panel never became visible. A second run, with the node's state logged, showed why: `DockBuilderGetNode(kBottomPanel)` is null in the headless lane (`bottom node=0`), and no side bar is open either. `reset_layout_docking.test.cpp` records the same property: Mesa-headless does not realise the canonical dock nodes even when healthy. The case now resizes whichever edge panel is visible, sees the nudge through the panel's drag state, and **skips with a logged reason when the headless software-GL lane realises no node** (it fails on a real-GL run). This is a coverage gap, not a pass: the key-to-nudge path is not exercised end to end in CI. The nudge's bounds moved into `SmatchetBottomPanelDragPure.h` (`NudgeTargetExtent`) and are covered by doctests; the missing infrastructure is filed as `docs/self-improvement/categories/tooling/2026-11-17-bucket-e-headless-lane-realises-no-edge-panel.md`.
  - The earlier theory, that a docked tab needed selecting, was wrong: an unselected docked tab keeps the node visible.
- **Deferred, as planned:** grid column move / resize (`grid.column.*`), the sort-rule list (#71), the views-dashboard row-reorder rebind, the palette outside-click close, and the filter + list popups onto `FilterDropdown` (#119). The check script lists the three still-unmarked drag sources as WARN-only pending entries; `--strict` fails them and is for the follow-up to turn on. A backlog entry covers column resize on the read-only tables.
- **The follow-up work moved to a debt entry** (`2026-11-22-gesture-keys-deferred.md`) instead of a second plan, so this plan could be archived.

## Verification (actual)

- **Bucket A, run locally (Linux clang):**
  - `ninja-test-linux` doctest subset: 1184 of 1184 passed, including the 8 new `GestureKeysPure` cases, the `TrackerDateTimePure` date-step cases and the `NudgeTargetExtent` bounds case.
  - `KeybindingsConfig.test.cpp` (defaults parity with the seven new rows, plus the hotkey round-trip), built on its own: 19 of 19 passed.
  - `bash scripts/dev/test-drag-keyboard-equivalent.sh --selftest` (21 cases), the real-tree `--check`, and `test-drag-keyboard-equivalent-bats.sh` (7 of 7).
  - `pre-ship.sh`: delta lint, comment-noise, function size, duplication, include-cycle, localization drift, markdown lint, test-list, orphan-bats and `test-docs.sh` all passed.
  - `posix-core-check` compiled every touched Core TU; the UI-test TUs (`gesture_keys.test.cpp`, the toolbar, annotate-prefs and MCP seams) were syntax-checked with `-DSMATCHET_BUILD_UI_TESTS`.
- **Not run locally:** `ConfigMigration.test.cpp` (the three new `migrated_gesture_keys_v1` cases), `SmatchetWorklogSubmitPure.test.cpp` (the modal-allow cases) and `LuaConsolePlugin.cpp` need the full MSVC test rig. The bucket-E suite `tests/ui/gesture_keys.test.cpp` (8 cases) needs the Windows Mesa lane. All of these are verified by CI on this PR, see below.
- **Review:** four independent code-review passes against the project's gate. They found one HIGH in the check script (a wrapped marker made it fail on its own tree), one HIGH in a new test (a command dispatched from the coroutine thread), one MEDIUM (the selection deviation above) and a handful of LOWs. All were fixed or accepted; the final artifact is 0 critical, 0 high, 0 medium, 1 accepted low.
- **Bucket-E on the code head `933b940`:** "Bucket-E UI tests (Mesa headless GL)" passed, and so did the Jira fixture-backend bucket-E lane, bucket-C, launch-smoke, TSan, the MSVC ASan lane and both MSVC builds. The 7 gesture-key cases that can run there pass; `PanelResize_KeysNudgeAVisiblePanel` takes its logged precondition skip (see the coverage gap below). Earlier heads failed this lane twice; the causes (Alt-release menu-layer toggle, panel visibility, missing cleanup on early return) are in § Deviations.
- **Bucket-E on head `4adb75f` (plan-doc commit; code identical to `933b940`):** the first attempt failed 249 of 251. The two failures were `WindowExpand/SplitterDragAfterMinimizeKeepsDocking` (`window_expand.test.cpp:708`) and `WindowExpand/ScriptingWindowContributesTabBarToggle` (`:792`), both dock-node preconditions; every `GestureKeys` case passed. `WindowExpand` is registered before `GestureKeys`, so the new tests' cleanup cannot have reached them. A re-run of the failed job on the same head passed, which makes it a confirmed flake, logged as the tooling backlog entry `2026-11-17-bucket-e-windowexpand-dock-flake`. On the same head, UBSan (Clang), ASan (MSVC), the Jira fixture-backend bucket-E lane, bucket-C, launch-smoke and both MSVC builds also passed. UBSan had not completed on `933b940` (superseded by the push), so `4adb75f` is its first completed run for this code.
- **Mutation check of the bucket-E tests: not run.** Reverting a consumer's take, a command's post or a default binding on a scratch commit needs a CI run per mutation, and the brief allows pushes only to this PR's branch, so a scratch commit would land on the PR itself. This container cannot run bucket-E. What exists instead: each case asserts an observable effect (list order and selection, splitter extent, calendar date, focused field), so a reverted branch should leave the asserted value unchanged. That is reasoning, not a measured red. A reviewer with a scratch branch can close this; it is the one verification item this plan does not claim.
- **`grill-with-docs` stress-test:** the plan's terms were checked against the audit's § 3 contract and the Ui and Commands leaf docs during the review passes; the contract rule in § 3 and `DialogKeysPure.h` now say the same thing.
- **Known coverage gap:** `PanelResize_KeysNudgeAVisiblePanel` skips (logged) in the headless lane, where no dock node is realised, so the key-to-panel-resize path is not exercised end to end in CI and that case has no mutation check. Command registration, the default bindings, the `panel=focused`-with-no-panel result and the nudge's bounds (`NudgeTargetExtent` doctests) are covered. The missing infrastructure is the tooling backlog entry `2026-11-17-bucket-e-headless-lane-realises-no-edge-panel`; a real-GL bucket-E run or a manual resize of a panel with the default keys closes it until then.
- **Review after the develop merges (2026-11-20):** develop was merged into the branch five times since the earlier review (four by another session; the fifth, which brought in #2441, merged with no conflicts). The other session also resolved conflicts in the config seeds, the keyboard-shortcuts doc, the path-remap editor and the toolbar, and changed `PanelResize_KeysNudgeAVisiblePanel` (it now closes leftover popups and rebuilds the default layout when no bottom dock node exists). An independent reviewer, in a fresh context, reviewed the full branch diff against develop and focused on the 14 files whose PR-side content changed since the earlier review; it found 0 critical, 0 high, 0 medium and 1 low. It checked the seed-migration order and the tests' teeth, default-key collisions (by reading `KeybindingsConfig::Defaults()` and the exact-modifier matcher), the panel-resize test's threading and cleanup, the conflict resolutions and the project invariants. Separately, a script scan of the default chords (run by the author, not the reviewer) found 47 chord rows and none bound to two commands.
  - **Accepted low, carried, not fixed here:** `MigrateGestureKeybindingsV1` (like the quick-create, grid-search and action-keys seeds) only runs when the config JSON is non-empty, so on a fresh profile the flag is first saved as false. A gesture row the user clears with the quick-bind Clear button before the flag is ever set comes back on the next launch (most likely the bare `Delete` row for `ui.list.remove`). It is not new to this change, and one fix (set the seed flags to true when Load starts from `Defaults()` with no config file) covers all four seeds, so it belongs in its own change.
  - **Local checks on the merged tree:** `posix-core-check` build clean, `ninja-test-linux` 1331 of 1331, `test-drag-keyboard-equivalent.sh` pass with the same four pending sources.
- **Manual residue:** one, the panel-resize keys (above). The other bucket-E cases are verified by the green CI run on code head `933b940`, recorded above.
