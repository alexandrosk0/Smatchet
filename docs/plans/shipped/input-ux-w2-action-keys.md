# Plan — Input UX Wave 2 piece 4.4: a key for every primary action
<!-- plan-date: 2026-11-19 -->

> **Slug**: `input-ux-w2-action-keys` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — Wave 2 piece 4.4, PR #2443 (merged as `8cf3bb91a`); the row-14 sidebar Enter and the grid-header view-selector hint landed after the sibling plan-lock released (see § Deviations).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

`docs/audits/INPUT_UX_AUDIT.md` § 4.4 ("A key for every primary action") groups thirteen findings where submit, save, search, run or pick has no key, or has one that nothing on screen names: #6, #49, #59, #60, #65, #72, #79, #95, #97, #99, #105, #107, #111 (detail in § 7).

Wave 1 already closed four of them, and they are **not** reworked here:

- #49 (new-issue row Tab / Ctrl+Enter / Esc) and #60 (`grid.search.focus`, Ctrl+F and /) ship in #2435 (`claude/input-ux-wave1-stack`).
- #72 (Preferences Ctrl+S, Enter tests the connection) and #97 (assistant Esc stops or leaves) ship in #2433 (merged; it was this branch's base).
- Wave 1 also gave Comments (#48) and Log work (#37) their Ctrl+Enter, which leaves #6 with only the placeholder and the button hint.

After this lands: every multiline composer that submits names its key **on the submit button**, its placeholder sits inside the box, the remaining primary actions (settings search, next/previous view, Lua Run, Annotate Process, the conflict resolver's Use Mine / Use Theirs, the dictation cancel) have a key, the global ones are registered rebindable commands listed in the shortcut guide, and `docs/guides/keyboard-shortcuts.md` states the dialog key rules.

## Approach

**One helper, two widgets** (`Ui/SmatchetSubmitKeys.h`). `SubmitMultilineInput` draws a multiline field through the localized ImGui wrapper (so the dictation hook still registers the buffer), with `EnterReturnsTrue` so Enter types a new line and Ctrl+Enter returns true, never `AllowTabInput`, and draws its placeholder inside the box while the buffer is empty (multiline has no native hint). `KeyHintButton` draws a normal button whose label is left-aligned and whose key name (for example `Ctrl+Enter`) is drawn dim at the right edge inside the frame, widening the button to fit — the way menu items print their shortcut. The button id stays the label, so callers and tests that find a button by label keep working. The layout arithmetic and the placeholder rule live in an ImGui-free `Ui/SubmitKeysPure.h` for doctest.

**Global actions become registered commands** (one registry, every surface): `preferences.search.focus` (open Preferences and put the caret in its search box; default `Ctrl+Shift+,`, next to `Ctrl+,`), `view.next` / `view.previous` (cycle the active view in the order the view list shows; defaults `Ctrl+PageDown` / `Ctrl+PageUp`). Existing configs get the three bindings once through a one-shot migration (`migrated_action_keys_v1`), the same replace-not-merge pattern as `migrated_quick_create_hotkey_v1`. `Ctrl+F` stays `grid.search.focus`'s (#2435); routing `Ctrl+F` to the settings search while Preferences has focus would make both commands fire on one key until a context-routing rule exists, so this plan does not bind it.

**Window-local primary actions keep fixed keys**, like the existing Preferences `Ctrl+S` and the dialog keys: they act on the window that has focus and mean nothing elsewhere, so a global command would only fire them from the wrong place. They are documented in the shortcut guide's fixed-key tables and printed on their buttons: Lua console Run (`Ctrl+Enter` / `F5` while the Scripts window has focus), Annotate Process (`Ctrl+Enter` in the callstack box), conflict resolver Use Mine / Use Theirs (`Alt+M` / `Alt+T` while the resolver has the keyboard), dictation cancel (`Esc` while recording discards the clip).

Dialog-scope work (focus the first field, route Esc/Enter, mute global shortcuts, the confirm footer, an `Esc` hint on Cancel) is piece 4.1's and is not built here; `KeyHintButton` is the primitive 4.1 can reuse for its Esc hint.

## Interface contracts

- **New public helpers** (`Source/Core/include/Ui/SmatchetSubmitKeys.h`, namespace `smatchet::ui`):
  - `bool SubmitMultilineInput(const char* label, char* buf, std::size_t bufSize, const ImVec2& size, const char* placeholder, ImGuiInputTextFlags extraFlags = 0, bool dictation = true);` — `dictation` registers the buffer with the dictation router like every localized InputText; pass false for a buffer that is not process-static (Annotate's, which the `AnnotateAnalysisUi` instance owns). True on the frame Ctrl+Enter (or keypad Ctrl+Enter, or the gamepad validate) was pressed with the caret in it; ImGui deactivates the field on that frame, as it does today for `EnterReturnsTrue`. `extraFlags` may not carry `AllowTabInput` (masked off: Tab moves on, per the § 3 contract).
  - `bool KeyHintButton(const char* label, const char* keyHint, const ImVec2& size = ImVec2(0, 0));` — `size.x` is a minimum width; a null or empty `keyHint` draws a plain button.
  - `const char* CtrlEnterKeyName();` — localized `Ctrl+Enter` (`keys.ctrl_enter`).
  - UI-test seam, compiled only with `SMATCHET_BUILD_UI_TESTS`: `std::string KeyHintDrawnForTest(const char* buttonLabel)` and `bool PlaceholderDrawnForTest(const char* fieldLabel)` — what the current or previous frame drew, keyed by the label the caller passed (both are draw-list text, not items, and a test cannot easily rebuild the id of a widget deep in a popup).
- **Pure layout** (`Source/Core/include/Ui/SubmitKeysPure.h`, namespace `smatchet::submitkeys`): `KeyHintButtonLayout ComputeKeyHintButtonLayout(float labelW, float hintW, float gap, float padX, float minW)` (width + hint x offset) and `bool ShowsPlaceholder(const char* buf)`.
- **Commands** (`RegisterCommand` entries, CLI + palette + MCP + Lua): `preferences.search.focus` (no params; returns `{requested: true}`; non-destructive, idempotent); `view.next` / `view.previous` (no params; returns `{activated: <id>}`; `NotFound`-class failure when no view exists; same activate + save + sync path as `view.activate`).
- **Pure view cycle** (`Source/Core/include/ViewCyclePure.h`, namespace `smatchet::views`): `int CycleViewIndex(int count, int activeIndex, int step)` — wraps; an unknown active view (`-1`) lands on the first (step > 0) or last (step < 0).
- **Preferences search focus seam** (`Source/Core/include/Ui/SmatchetPrefsSearchFocus.h`): `void RequestPrefsSearchFocus();` — UI-thread only, consumed by the next Preferences draw.
- **`PreferencesFilter`** (`Source/Core/include/Ui/PreferencesFilter.h`): `void RequestFocusOnFirstMatch(void (*focusHook)())` — arms a one-draw request; the first `ShowSetting` that returns true for a real match calls the hook (the UI passes a `SetKeyboardFocusHere` thunk) and disarms; the next `Update` disarms an unspent request. Stays ImGui-free.
- **`DictationInsertionRouter`** (both the Whisper and the stub TU): `void RequestDiscard()` (sets the discard flag and stops the recording indicator), `bool TakeDiscardRequest()` (exchange-false), `void ClearDiscardRequest()`. Atomic, lock-free, hook-thread safe.
- **Config format** (additive only): `migrated_action_keys_v1` (bool) in `smatchet_config.json`; `TrackerConfig::MigratedActionKeysV1`.

## Files to modify

**Helper (new)**

1. `Source/Core/include/Ui/SubmitKeysPure.h` (new) — layout + placeholder rule.
2. `Source/Core/include/Ui/SmatchetSubmitKeys.h` (new) — the two widgets, the key name, the UI-test seam.
3. `Source/Core/src/Ui/SmatchetSubmitKeys.cpp` (new) — routed through `SmatchetLocalizedImGui` for labels and the dictation hook.

**Composers and buttons (#6, #59, #95, #107)**

4. `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` — post box through `SubmitMultilineInput` (placeholder inside the box, replacing the line under it), Post Comment through `KeyHintButton`; the separate "Ctrl+Enter to post" line goes.
5. `Source/Core/src/TicketFieldEditor_Worklog.cpp` — description through `SubmitMultilineInput` with a placeholder; Save through `KeyHintButton`.
6. `Source/Core/src/Ui/SmatchetBugReportUi.cpp` — Submit through `KeyHintButton` (its Ctrl+Enter already exists).
7. `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp` — callstack box through `SubmitMultilineInput`; Ctrl+Enter runs `RunAnnotateProcessFromBuffers` when idle; Process prints the key.

**Global commands (#65, #79)**

8. `Source/Core/src/Commands/ViewCommands.cpp` — `view.next` / `view.previous`, sharing a helper with `view.activate`. With unapplied edits in the Views editor (a draft for the active view that differs from it) they do not switch: they raise the same "Discard changes?" confirm a sidebar click does, bring the dashboard forward, and return `{activated: null, pendingConfirm: <id>}`. The check is the pure `ViewDraftWouldLoseEditsOnSwitch` (`ViewColumnsPure.h`): it rebases copies of the draft onto the saved view, as the dashboard does before a sidebar click, so a grid drag or a `view.update` is not read as an edit, and it counts a rename still being typed in the title box.
9. `Source/Core/include/ViewCyclePure.h` (new) — `CycleViewIndex`.
10. `Source/Core/src/Commands/Builtin/BuiltinCommands_Ui.cpp` — `preferences.search.focus`.
11. `Source/Core/include/Ui/SmatchetPrefsSearchFocus.h` (new) — the request seam.
12. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` — take the request; Esc in the search box clears it and leaves; Enter puts the caret on the first matching setting; the query clears when the window closes; the placeholder names the bound key.
13. `Source/Core/include/Ui/PreferencesFilter.h`, `Source/Core/src/Ui/PreferencesFilter.cpp` — `RequestFocusOnFirstMatch`.
14. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` — Enter in the sidebar search activates the first listed match; the search box's tooltip names the view keys. **Landed after #2443** (see § Deviations from plan): the file was in the write set of plan-lock `pane-picker-jira-sites` when #2443 shipped.
15. `Source/Core/src/Config/KeybindingsConfig.cpp` — three default bindings, appended after the quick-create row.
16. `Source/Core/include/Config/ConfigManager.h`, `Source/Core/src/Config/ConfigManager_Load.cpp`, `Source/Core/src/Config/ConfigManager_Save.cpp` — `migrated_action_keys_v1`.

**Window-local keys (#99, #105, #111)**

17. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` — `Alt+M` / `Alt+T` in `ReadConflictModalKeys`; one `DrawConflictPickButtons` used by the text and scalar panes; the buttons print their keys.
18. `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp` — Ctrl+Enter / F5 run the row's primary button while the Scripts window has focus (not while the autocomplete popup has it); the button prints `Ctrl+Enter`; the hint line names it.
19. `Source/Core/include/DictationInsertionRouter.h`, `Source/Core/src/DictationInsertionRouter_Whisper.cpp`, `Source/Core/src/DictationInsertionRouter_Stubs.cpp` — the discard flag.
20. `Source/Core/src/Ui/SmatchetWhisperOverlayUi.cpp` — Esc while recording and the Cancel button both call `RequestDiscard`.
21. `Source/Plugins/Whisper/WhisperPlugin.cpp` — press clears a stale discard; both release paths drop the clip when a discard was requested (no transcription, no insertion, no failure toast).

**Docs**

22. `docs/guides/keyboard-shortcuts.md` — a Dialogs section with the § 3 key rules; the new defaults; the fixed keys (Lua, Annotate, resolver picks, dictation, settings search Esc/Enter); the submit-button hint convention.
23. `docs/guides/cli.md` — `view.next` / `view.previous` / `preferences.search.focus` rows.

**Tests** (existing, already-registered files only — see § Risks, plan-lock)

24. `tests/Core/SmatchetIconButtons.test.cpp` — `SubmitKeysPure` cases.
25. `tests/Core/ViewsClass.test.cpp` — `CycleViewIndex` cases; `tests/Core/ViewColumnsPure.test.cpp` — `ViewDraftWouldLoseEditsOnSwitch` cases.
26. `tests/Core/PreferencesFilter.test.cpp` — first-match focus request.
27. `tests/Core/DictationInsertionRouter.test.cpp` — discard flag.
28. `tests/Core/KeybindingsConfig.test.cpp`, `tests/Core/ConfigMigration.test.cpp` — defaults parity, migration seed.
29. Bucket-E cases appended to `tests/ui/field_viewer_modal.test.cpp` (comments), `tests/ui/duration_inline_edit_commit.test.cpp` (worklog), `tests/ui/offline_conflict_modal_panes.test.cpp` (resolver picks), `tests/ui/prefs_search_filter.test.cpp` (settings search), `tests/ui/views_editor_header.test.cpp` (view keys and the dirty-draft confirm).

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Ui/SmatchetSubmitKeys.cpp` | `KeyHintButton` | `CORE_SOURCES` (glob) |
| `Source/Core/src/Commands/ViewCommands.cpp` | `RegisterViewActivateCommand` | `CORE_SOURCES` (glob) |
| `Source/Core/src/Commands/Builtin/BuiltinCommands_Ui.cpp` | `RegisterUiInteractionCommands` | `CORE_SOURCES` (glob) |
| `Source/Core/src/Ui/PreferencesFilter.cpp` | `PreferencesFilter::ShowSetting` | `CORE_SOURCES` (glob) |
| `Source/Core/src/Config/ConfigManager_Load.cpp` | `MigrateQuickCreateKeybindingV1` | `CORE_SOURCES` (glob) |
| `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` | `ReadConflictModalKeys` | `CORE_SOURCES` (glob) |
| `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp` | `DrawRunButtonRow` | — |

## Existing utilities reused

- `SmatchetLocalizedImGui::InputTextMultiline` / `Button` (`Source/Core/include/SmatchetLocalizedImGui.h:65,132`) — label translation and the dictation hook, kept by routing the helper through them.
- `smatchet::dialogkeys::ResolveEscape` (`Source/Core/include/Ui/DialogKeysPure.h`) — the resolver already reads its keys through it; the pick keys sit beside it in `ReadConflictModalKeys`.
- `BoundHotkeyDisplay` (`Source/Core/include/Config/KeybindingsConfig.h:102`) — the live bound combo for placeholders, so a rebind is reflected next frame.
- `MigrateQuickCreateKeybindingV1` (`Source/Core/src/Config/ConfigManager_Load.cpp`) — the one-shot seed pattern copied for `migrated_action_keys_v1`.
- `RunOnUiThreadAsCommandResult` (`Commands/MainThreadDispatch.h`) — every new handler hops to the UI thread.
- `Views::Activate` / `Views::Save` / `AppController::SyncWithBackend` — the `view.activate` path, shared rather than copied.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no file is split. The new helper TU is small; touched TUs stay under the 1,200-line ceiling or were already over it (grandfathered).

## UX Pillar callouts

- **Pillar 1 (perf)**: the helper adds one `CalcTextSize` per hinted button and one draw-list text per empty composer per frame; no per-cell grid path is touched. The UI-test seam maps exist only in the UI-test build.
- **Pillar 2 (UI-thread never blocks)**: no new I/O. `view.next` / `view.previous` reuse `view.activate`'s save + async sync. The dictation discard is an atomic flag.
- **Pillar 3 (never crash)**: no new owning pointers; buffers stay caller-owned; `SubmitMultilineInput` forwards the caller's size. The router flag is `std::atomic<bool>`.
- **Pillar 4 (accessibility)**: the point of the change — every primary action gets a key and the button names it.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires as usual; no scenario targets these dialogs, and the grid frame path is untouched.
2. **Pillar 2 static scanner** — N/A: no sync I/O added under `ImGui::*`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` added.

## Risks / non-goals

- **Plan-lock contention.** Other branches hold `SmatchetLocalization.cpp`, `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`, `SmatchetUiSession.h`, `SmatchetGridHeaderUi.cpp` and `SmatchetQuickCreateIssueUi.cpp`. Mitigation: tests go into already-registered test files, session state stays file-local, the grid-header and quick-create button hints wait, and the fr-FR rows for the new keys are added once `SmatchetLocalization.cpp` is free (English fallbacks render until then, as Wave 1 did).
- **Merge with #2435.** It edits `KeybindingsConfig.cpp`, `ConfigManager*` and `BuiltinCommands_Ui.cpp` in nearby lines; the new rows are placed in separate hunks (after the quick-create row, after the existing migrations). Accepted.
- **A key must not skip a guard the pointer path has.** `Ctrl+PageDown` is typing-safe, so it fires while the caret is in the Views editor's query field. `view.activate` (CLI, palette, MCP) has always switched without the unsaved-changes confirm; the step commands do not inherit that gap (review finding, fixed). `view.activate` itself is unchanged.
- **`Ctrl+PageUp` / `Ctrl+PageDown`** also page-scroll an ImGui window that has keyboard nav. Accepted: the combo is what browsers and editors use for tab switching; rebindable.
- **`Alt+M` / `Alt+T` on macOS** type `µ` / `†` into a focused field as well. Accepted: the pick replaces the field anyway.
- **Esc during dictation** also reaches the window that has the keyboard, because the overlay is drawn after the windows and reads Esc app-wide. With text typed, the field is only left and nothing is lost. On an untouched dialog (an empty comment box, an untouched Log work) the same Esc also closes it. Accepted for now: nothing typed is lost either way. Taking Esc for the overlay before the dialogs read it needs key-ownership routing (`SetKeyOwner` with `LockThisFrame` early in the frame), which cannot be exercised without a bucket-E run; follow-up.
- **Dictation registration of the comments buffer.** `PostBuf` was already registered with the dictation router through the localized wrapper. The reset that frees it now unregisters it first (`ResetCommentsState`), so the router's sticky target cannot point at freed memory.
- **Bucket-E cases that close Preferences.** Preferences, Log and Scripting share the bottom-panel dock node, and in a fresh test home Preferences is its only tenant. The first-run lock normally keeps it open; the settings-search cases lift the lock to prove a real close, and ImGui deletes a node when its last window leaves, which left every later `WindowExpand` case with no bottom panel (Log floating, Scripting undocked; the two failures in the first CI runs). Each case now keeps the Log window open while it may close Preferences (`SearchKeysCase::KeepBottomPanelAlive`) and gives it back at the end.
- **Dual target**: the helper header includes only `imgui.h`; nothing GLFW/GL reaches `Source/Core/include/`.
- Non-goals: dialog scope / first-field focus / Esc hints on Cancel (4.1); Lua autocomplete auto-open and fuzzy ranking, the views header combo filter row and a `view.switcher` palette scope (#105 / #65 remainders, 4.6 / 4.7); a live recognised-frame count for Annotate (4.5); a context-routed `Ctrl+F` for the settings search.

## Implementation handoff

N/A — single slice, orchestrator (Ui leaf + Commands leaf + Config). The Commands part is the three registrations only; the Ui part is everything else. They ship together because the bindings name the commands.

## Verification

- **Bucket A (doctest)**: `SubmitKeysPure` layout (width grows to fit, min width honoured, empty hint, hint x at the right edge) and `ShowsPlaceholder`; `CycleViewIndex` (wrap both ways, unknown active, empty list, single view); `PreferencesFilter::RequestFocusOnFirstMatch` (hook fires once on the first real match, not on an empty query, not on a fail-open unknown id, disarmed by the next `Update`); router discard flag (take clears, clear resets, request stops recording); defaults parity rows; migration seeds the three rows once and keeps a user binding.
- **Bucket E (ImGui Test Engine)**: Comments — Post Comment shows `Ctrl+Enter`, placeholder drawn in the empty box and gone once typed; Log work — Save shows `Ctrl+Enter`, Ctrl+Enter in the description saves; resolver — `Alt+M` / `Alt+T` resolve to mine / theirs and the buttons show the keys; settings search — `preferences.search.focus` and its chord put the caret in the box, Esc clears, Enter lands on the first match, closing clears the query; views — `Ctrl+PageDown` / `Ctrl+PageUp` move the active view, and with a dirty editor draft ask "Discard changes?" and keep the draft. Each new test is mutation-checked (the guard broken on purpose, the test seen red).
- **Bash-driver scenario / screenshot / sanitizer**: N/A — covered by bucket E; bucket-C goldens may shift where a button widened (reported, mask-documented).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on CI; locally the `posix-core-check` compile of `Source/Core` and a scratch doctest compile of the pure headers.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against this plan before the PR leaves draft scope; outcome recorded under § Deviations.
- **Manual residue**: none planned.

## Open questions

None. The `Ctrl+F` routing question is settled for this PR (not bound; follow-up once #2435 lands).

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Quick-create Create button hint — `SmatchetQuickCreateIssueUi.cpp` was locked, then edited by open PR #2450; tracked by debt entry `2026-11-20-input-ux-w2-held-hints-and-fr-rows`. (The grid header view-selector tooltip landed after #2443: § Deviations.)
- Context-routed `Ctrl+F` (settings search while Preferences has focus, grid search elsewhere) — follow-up after #2435.
- Lua autocomplete auto-open / fuzzy (#105 remainder) — 4.7 matcher.
- Views header combo filter row and `view.switcher` palette scope (#65 remainder) — 4.6.
- Annotate recognised-frame count (#107 remainder) — 4.5.

## Implementation log

- `5ea1be7` · wip(plan): this plan.
- `b8b77bd` · a key for every primary action, named on its button: `SubmitMultilineInput` / `KeyHintButton`, the three new rebindable commands and their migration, the window keys, the docs, doctests and bucket-E cases.
- `187f2a4` · merge of `origin/develop` once #2433 landed (per-file three-way merge of the files both sides touched).
- `100797a` · `view.next` / `view.previous` ask "Discard changes?" before dropping unapplied editor edits (`ViewDraftWouldLoseEditsOnSwitch`, review finding).
- `68b5225` · test diagnostics: name the wait that failed when the Assistant page helper cannot open Preferences.
- `cec9eeb` · settings-search bucket-E cases stop leaving Preferences open (first-run lock, caret in the search box).
- `b8c35c8` · the views-sidebar Enter hunk is taken out while another plan-lock holds that file.
- `f8f7f2c` · the settings-search cases keep the Log window open while they close Preferences, so the shared bottom-panel dock node survives.

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

- **Deferred: Enter in the views sidebar search opens the first listed view (§ Files row 14).** After this PR's first push, another branch (`claude/plus-dropdown-two-jira-261a42`) claimed plan-lock `pane-picker-jira-sites`, whose write set includes `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp`. The hunk, its tooltip, its bucket-E section and its doc row were removed from this PR instead of overriding the lock. The `view.next` / `view.previous` commands and keys are unaffected.
- **Settings-search bucket-E cases needed a dock-node tenant.** The cases lift the first-run lock and really close Preferences, which is the only window in the bottom-panel dock node of a fresh test home; ImGui deletes a node when its last window leaves, so the next `WindowExpand` cases found no bottom panel (CI run 1 of `b8c35c8`: two failures). Not in the plan; fixed in `f8f7f2c` (`SearchKeysCase::KeepBottomPanelAlive`). Other tests that close Preferences with the lock lifted (`ai_prefs_autosave_flow`, `funcsize_preferences_tabs`, `prefs_schema_coverage`, `keybindings_editor_rebind`) were not audited for the same exposure; the lane is green at `f8f7f2c`, but a new Preferences-closing case placed before `WindowExpand` would hit the same trap (a shared fixture or an after-test `EnsureDockSlotAlive(kBottomPanel)` check would catch it; not built here).
- **Plan-lock not claimed.** `lock-claim.sh` could not push to `refs/locks/*` from this environment (HTTP 403 through the proxy). The PR was narrowed against sibling locks instead of overriding anything, and the CI `Plan-lock gate` passed.
- **Landed after #2443 (2026-11-20, input UX Wave 2 lock-deferrals PR, branch `claude/vigorous-khayyam-8e2d16`).** Once `pane-picker-jira-sites` was released: Enter in the views sidebar search opens the first listed view (now the matcher's best match, since the list ranks through `TextMatchPure.h`), the search box's tooltip names `view.next` / `view.previous` read live from the bindings, and the grid header's view selector tooltip names the same keys (`smatchet::views::ViewSwitchKeysTooltip`, `ViewPickerPure.h`, drawn by `Ui/SmatchetViewSwitchTooltip.h`). Only a bare Enter opens a view: Ctrl+Enter stays the dashboard's Apply & Sync key, and a blank search opens nothing. Doctests in `tests/Core/ViewColumnsPure.test.cpp`; bucket-E `ViewsEditorHeader/Sidebar_EnterOpensFirstMarksMatchRenameSelects`. The Quick-create Create button hint stays out (its file is in open PR #2450), and so does the shortcut guide's row for the views search (open PR #2439 edits the guide); both are in debt `2026-11-20-input-ux-w2-held-hints-and-fr-rows`.
- **fr-FR rows not added** for the new strings: `SmatchetLocalization.cpp` was held by other plan-locks and is now edited by open PR #2440, so English fallbacks render (debt `2026-11-20-input-ux-w2-held-hints-and-fr-rows`).
- **Plan stress-test (`grill-with-docs`)**: not run as its own pass; the plan was instead checked by eight gate-review rounds (two High findings and three Medium findings fixed, one claim about `SetKeyboardFocusHere` re-checked against ImGui 1.92.8 and rejected, one Medium accepted as the Esc-on-dictation risk above).
- **Archive deferred, then done.** #2443 could not move this plan to `shipped/` or add its `docs/plans/INDEX.md` row while plan-lock `pane-picker-jira-sites` held `INDEX.md`. The lock-deferrals PR above did both and archived debt entry `2026-11-19-archive-input-ux-w2-action-keys-plan`.
- **Deferral residue-sweep**: the quick-create hint and the fr-FR rows are the only items left; both are in debt entry `2026-11-20-input-ux-w2-held-hints-and-fr-rows`.

## Verification (actual)

- **Bucket A (doctest)**: `SubmitKeysPure`, `CycleViewIndex`, `ViewDraftWouldLoseEditsOnSwitch` (clean draft, edited draft, title being typed, rebased draft), `PreferencesFilter::RequestFocusOnFirstMatch`, the router discard flag, default-binding parity and the migration: passed locally in a scratch doctest harness (the Linux container cannot build the full suite) and in CI (Windows MSVC unit tests). Each guard was broken on purpose and the test seen red before the fix was restored: passed.
- **Bucket E (ImGui Test Engine)**: comments, Log work, resolver (`Alt+M` / `Alt+T`), settings search (chord, Esc, Enter, command, close clears) and the views keys including the dirty-draft "Discard changes?" case: passed in CI, `Bucket-E UI tests (Mesa headless GL)` at `f8f7f2c` = `252/252 passed` on attempt 1. The two earlier failing runs (`100797a`: the new search cases left Preferences open and used up the lane's frame budget, fixed in `cec9eeb`; `b8c35c8`: the dock node, under Deviations) came from the new tests' own state, not from a product defect.
- **Build gate**: `posix-core-check` (Linux clang) compile of `Source/Core` locally; the Windows MSVC, light, ARM64 cross-compile, DX12 packaging, NDK and Android APK builds passed in CI (the APK job at `b8c35c8` failed on a Maven Central 429, an infrastructure error; it passed on attempt 1 at `f8f7f2c`).
- **Lint / gates**: `scripts/dev/pre-ship.sh --ack-review origin/develop` passed at each pushed head; CI C++ lint, duplication, Pillar 2, comment-noise, plan-lock and doc gates passed.
- **Sanitizers / goldens**: TSan, ASan (MSVC), UBSan (Clang), bucket-C and the launch smoke passed in CI; the aggregate `All checks green (block-on-any-red)` passed at `f8f7f2c`.
- **Manual residue**: none.
