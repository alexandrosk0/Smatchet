# Plan — Input UX Wave 2 piece 4.4: a key for every primary action

> **Slug**: `input-ux-w2-action-keys` (matches this file's basename without `.md`).
>
> **Status**: `active` — driving the Wave 2 piece 4.4 PR (base `claude/frictionless-user-input-ux-ea174b` until #2433 merges, then `develop`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

`docs/audits/INPUT_UX_AUDIT.md` § 4.4 ("A key for every primary action") groups thirteen findings where submit, save, search, run or pick has no key, or has one that nothing on screen names: #6, #49, #59, #60, #65, #72, #79, #95, #97, #99, #105, #107, #111 (detail in § 7).

Wave 1 already closed four of them, and they are **not** reworked here:

- #49 (new-issue row Tab / Ctrl+Enter / Esc) and #60 (`grid.search.focus`, Ctrl+F and /) ship in #2435 (`claude/input-ux-wave1-stack`).
- #72 (Preferences Ctrl+S, Enter tests the connection) and #97 (assistant Esc stops or leaves) ship in #2433 (this branch's base).
- Wave 1 also gave Comments (#48) and Log work (#37) their Ctrl+Enter, which leaves #6 with only the placeholder and the button hint.

After this lands: every multiline composer that submits names its key **on the submit button**, its placeholder sits inside the box, the remaining primary actions (settings search, next/previous view, Lua Run, Annotate Process, the conflict resolver's Use Mine / Use Theirs, the dictation cancel) have a key, the global ones are registered rebindable commands listed in the shortcut guide, and `docs/guides/keyboard-shortcuts.md` states the dialog key rules.

## Approach

**One helper, two widgets** (`Ui/SmatchetSubmitKeys.h`). `SubmitMultilineInput` draws a multiline field through the localized ImGui wrapper (so the dictation hook still registers the buffer), with `EnterReturnsTrue` so Enter types a new line and Ctrl+Enter returns true, never `AllowTabInput`, and draws its placeholder inside the box while the buffer is empty (multiline has no native hint). `KeyHintButton` draws a normal button whose label is left-aligned and whose key name (for example `Ctrl+Enter`) is drawn dim at the right edge inside the frame, widening the button to fit — the way menu items print their shortcut. The button id stays the label, so callers and tests that find a button by label keep working. The layout arithmetic and the placeholder rule live in an ImGui-free `Ui/SubmitKeysPure.h` for doctest.

**Global actions become registered commands** (one registry, every surface): `preferences.search.focus` (open Preferences and put the caret in its search box; default `Ctrl+Shift+,`, next to `Ctrl+,`), `view.next` / `view.previous` (cycle the active view in the order the view list shows; defaults `Ctrl+PageDown` / `Ctrl+PageUp`). Existing configs get the three bindings once through a one-shot migration (`migrated_action_keys_v1`), the same replace-not-merge pattern as `migrated_quick_create_hotkey_v1`. `Ctrl+F` stays `grid.search.focus`'s (#2435); routing `Ctrl+F` to the settings search while Preferences has focus would make both commands fire on one key until a context-routing rule exists, so this plan does not bind it.

**Window-local primary actions keep fixed keys**, like the existing Preferences `Ctrl+S` and the dialog keys: they act on the window that has focus and mean nothing elsewhere, so a global command would only fire them from the wrong place. They are documented in the shortcut guide's fixed-key tables and printed on their buttons: Lua console Run (`Ctrl+Enter` / `F5` while the Scripts window has focus), Annotate Process (`Ctrl+Enter` in the callstack box), conflict resolver Use Mine / Use Theirs (`Alt+M` / `Alt+T` while the resolver has the keyboard), dictation cancel (`Esc` while recording discards the clip).

Dialog-scope work (focus the first field, route Esc/Enter, mute global shortcuts, the confirm footer, an `Esc` hint on Cancel) is piece 4.1's and is not built here; `KeyHintButton` is the primitive 4.1 can reuse for its Esc hint.

## Interface contracts

- **New public helpers** (`Source/Core/include/Ui/SmatchetSubmitKeys.h`, namespace `smatchet::ui`):
  - `bool SubmitMultilineInput(const char* label, char* buf, std::size_t bufSize, const ImVec2& size, const char* placeholder, ImGuiInputTextFlags extraFlags = 0);` — true on the frame Ctrl+Enter (or keypad Ctrl+Enter, or the gamepad validate) was pressed with the caret in it; ImGui deactivates the field on that frame, as it does today for `EnterReturnsTrue`. `extraFlags` may not carry `AllowTabInput` (masked off: Tab moves on, per the § 3 contract).
  - `bool KeyHintButton(const char* label, const char* keyHint, const ImVec2& size = ImVec2(0, 0));` — `size.x` is a minimum width; a null or empty `keyHint` draws a plain button.
  - `const char* CtrlEnterKeyName();` — localized `Ctrl+Enter` (`keys.ctrl_enter`).
  - UI-test seam, compiled only with `SMATCHET_BUILD_UI_TESTS`: `std::string KeyHintDrawnForTest(ImGuiID buttonId)` and `bool PlaceholderDrawnForTest(ImGuiID fieldId)` — what the last frame drew, since both are draw-list text, not items.
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

8. `Source/Core/src/Commands/ViewCommands.cpp` — `view.next` / `view.previous`, sharing a helper with `view.activate`.
9. `Source/Core/include/ViewCyclePure.h` (new) — `CycleViewIndex`.
10. `Source/Core/src/Commands/Builtin/BuiltinCommands_Ui.cpp` — `preferences.search.focus`.
11. `Source/Core/include/Ui/SmatchetPrefsSearchFocus.h` (new) — the request seam.
12. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` — take the request; Esc in the search box clears it and leaves; Enter puts the caret on the first matching setting; the query clears when the window closes; the placeholder names the bound key.
13. `Source/Core/include/Ui/PreferencesFilter.h`, `Source/Core/src/Ui/PreferencesFilter.cpp` — `RequestFocusOnFirstMatch`.
14. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` — Enter in the sidebar search activates the first listed match; the placeholder names the view keys.
15. `Source/Core/src/Config/KeybindingsConfig.cpp` — three default bindings, appended after the quick-create row.
16. `Source/Core/include/Config/ConfigManager.h`, `Source/Core/src/Config/ConfigManager_Load.cpp`, `Source/Core/src/Config/ConfigManager_Save.cpp` — `migrated_action_keys_v1`.

**Window-local keys (#99, #105, #111)**

17. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp` — `Alt+M` / `Alt+T` in `ReadConflictModalKeys`; one `DrawConflictPickButtons` used by the text and scalar panes; the buttons print their keys.
18. `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp` — Ctrl+Enter / F5 run the row's primary button while the Scripts window has focus (not while the autocomplete popup has it); the button prints `Ctrl+Enter`; the hint line names it.
19. `Source/Core/include/DictationInsertionRouter.h`, `Source/Core/src/DictationInsertionRouter_Whisper.cpp`, `Source/Core/src/DictationInsertionRouter_Stubs.cpp` — the discard flag.
20. `Source/Core/src/Ui/SmatchetWhisperOverlayUi.cpp` — Esc while recording and the Cancel button both call `RequestDiscard`.
21. `Source/Plugins/Whisper/WhisperPlugin.cpp` — press clears a stale discard; both release paths drop the clip when a discard was requested (no transcription, no insertion, no failure toast).

**Docs**

22. `docs/guides/keyboard-shortcuts.md` — a Dialogs section with the § 3 key rules; the new defaults; the fixed keys (Lua, Annotate, resolver picks, dictation, settings search Esc/Enter, views sidebar Enter); the submit-button hint convention.
23. `docs/guides/cli.md` — `view.next` / `view.previous` / `preferences.search.focus` rows.

**Tests** (existing, already-registered files only — see § Risks, plan-lock)

24. `tests/Core/SmatchetIconButtons.test.cpp` — `SubmitKeysPure` cases.
25. `tests/Core/ViewsClass.test.cpp` — `CycleViewIndex` cases.
26. `tests/Core/PreferencesFilter.test.cpp` — first-match focus request.
27. `tests/Core/DictationInsertionRouter.test.cpp` — discard flag.
28. `tests/Core/KeybindingsConfig.test.cpp`, `tests/Core/ConfigMigration.test.cpp` — defaults parity, migration seed.
29. Bucket-E cases appended to `tests/ui/field_viewer_modal.test.cpp` (comments), `tests/ui/duration_inline_edit_commit.test.cpp` (worklog), `tests/ui/offline_conflict_modal_panes.test.cpp` (resolver picks), `tests/ui/prefs_search_filter.test.cpp` (settings search), `tests/ui/views_editor_header.test.cpp` (view keys + sidebar Enter).

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
- **`Ctrl+PageUp` / `Ctrl+PageDown`** also page-scroll an ImGui window that has keyboard nav. Accepted: the combo is what browsers and editors use for tab switching; rebindable.
- **`Alt+M` / `Alt+T` on macOS** type `µ` / `†` into a focused field as well. Accepted: the pick replaces the field anyway.
- **Esc during dictation** also reaches the focused field (it leaves it). Accepted: the clip is the user's target and nothing typed is lost.
- **Dual target**: the helper header includes only `imgui.h`; nothing GLFW/GL reaches `Source/Core/include/`.
- Non-goals: dialog scope / first-field focus / Esc hints on Cancel (4.1); Lua autocomplete auto-open and fuzzy ranking, the views header combo filter row and a `view.switcher` palette scope (#105 / #65 remainders, 4.6 / 4.7); a live recognised-frame count for Annotate (4.5); a context-routed `Ctrl+F` for the settings search.

## Implementation handoff

N/A — single slice, orchestrator (Ui leaf + Commands leaf + Config). The Commands part is the three registrations only; the Ui part is everything else. They ship together because the bindings name the commands.

## Verification

- **Bucket A (doctest)**: `SubmitKeysPure` layout (width grows to fit, min width honoured, empty hint, hint x at the right edge) and `ShowsPlaceholder`; `CycleViewIndex` (wrap both ways, unknown active, empty list, single view); `PreferencesFilter::RequestFocusOnFirstMatch` (hook fires once on the first real match, not on an empty query, not on a fail-open unknown id, disarmed by the next `Update`); router discard flag (take clears, clear resets, request stops recording); defaults parity rows; migration seeds the three rows once and keeps a user binding.
- **Bucket E (ImGui Test Engine)**: Comments — Post Comment shows `Ctrl+Enter`, placeholder drawn in the empty box and gone once typed; Log work — Save shows `Ctrl+Enter`, Ctrl+Enter in the description saves; resolver — `Alt+M` / `Alt+T` resolve to mine / theirs and the buttons show the keys; settings search — `preferences.search.focus` and its chord put the caret in the box, Esc clears, Enter lands on the first match, closing clears the query; views — `Ctrl+PageDown` / `Ctrl+PageUp` move the active view, Enter in the sidebar search activates the first match. Each new test is mutation-checked (the guard broken on purpose, the test seen red).
- **Bash-driver scenario / screenshot / sanitizer**: N/A — covered by bucket E; bucket-C goldens may shift where a button widened (reported, mask-documented).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) on CI; locally the `posix-core-check` compile of `Source/Core` and a scratch doctest compile of the pure headers.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against this plan before the PR leaves draft scope; outcome recorded under § Deviations.
- **Manual residue**: none planned.

## Open questions

None. The `Ctrl+F` routing question is settled for this PR (not bound; follow-up once #2435 lands).

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Quick-create Create button hint and the grid header view-selector tooltip — files locked by another branch; follow-up when free.
- Context-routed `Ctrl+F` (settings search while Preferences has focus, grid search elsewhere) — follow-up after #2435.
- Lua autocomplete auto-open / fuzzy (#105 remainder) — 4.7 matcher.
- Views header combo filter row and `view.switcher` palette scope (#65 remainder) — 4.6.
- Annotate recognised-frame count (#107 remainder) — 4.5.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.**
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
