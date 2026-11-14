# Plan — Shared filter dropdown with recent picks
<!-- plan-date: 2026-11-13 -->

> **Slug**: `shared-dropdown-recents` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

## Context

User request (2026-11-13): "Every dropdown, like in assignee, needs to show up to the last 5 selections. Check to see any other cases like that." Follow-up in the same session: "when we press any alphanumeric key while we are scrolling the dropdown should make it input for the filter. Make an UX pass on the dropdown to be modern."

An audit of `Source/` found 71 picker sites. None shares a dropdown helper: six sites re-implement "combo + filter `InputText` + `Selectable` loop", each with different focus, Enter and filter behaviour. There is no generic recents store; three unrelated MRUs exist (`CommandRegistry` recents, `RecentViewLru`, the field-catalog project LRU). Only the duration popup forwards typed characters to its input. The single-select cell editor also has a defect: `SpreadsheetState::SingleSelectActiveKey` is never cleared on close, so reopening the same cell neither refocuses the filter nor clears it.

After this lands, every tracker value picker in scope draws through one shared dropdown body that pins up to five recent picks above the list, sends any typed character to the filter wherever focus is, supports full keyboard navigation and has one consistent look.

### Decisions (user, 2026-11-13)

| Question | Decision |
|---|---|
| Scope | Value pickers: grid single-select (assignee, reporter, sprint, priority, status, issue type, custom select), grid multi-select (components, fix versions, user-multi), labels, new-issue option combo, project picker (3 call sites), JQL project pill, grid view selector, quick-create issue type. Settings combos untouched. |
| Small lists | Recent section shows only when the picker has more than 10 options. |
| Keying | One list per field per tracker instance (`<backendKey>|<fieldId>`), shared across projects. Recents absent from the current option list are hidden. |
| Persistence | Persisted in `smatchet_config.json` through the async config-save worker. |
| Display | "Recent" header and up to 5 rows pinned above the full list; recent items also stay in the full list; the section is hidden while a filter is typed. |
| Recording | On pick. Multi-select records each value ticked on; unticking records nothing. |
| UX pass | Full redesign: search field on top with icon and clear button, section headers, matched text highlighted, check mark on the current value, keyboard navigation, typed characters always reach the filter, keyboard-hint footer. |
| Project picker | Its "Recently used" section (a catalog-cache proxy) becomes the true last 5 picks from the shared store. Cached-catalog projects stay pickable in the full list. |

## Approach

Two new units at the `Source/Core` root (leaf utilities, usable from both the domain-side cell editors and the `Ui/` panels without breaking the `no-ui-include-in-domain` direction rule).

**`RecentSelections`** — an ImGui-free store: scope key → up to 5 ids, newest first, dedup-and-promote on record, bounded scope count (oldest-touched scope evicted). It serialises to one JSON object. A process-wide instance is loaded once at `AppController` init from the already-parsed config JSON and saved through `smatchet::config_save::EnqueueConfigJsonKey("recent_selections", …)`, so a pick never touches the disk on the frame thread and recents render offline from memory.

**`FilterDropdown`** — `smatchet::dropdown::DrawBody(items, options)` draws the body of an already-open popup (the caller keeps its `BeginCombo` / `BeginPopup`, so `ArmThenPopupCellGate` and existing popup ids are untouched). It owns per-popup state keyed by the popup window id and reset on `IsWindowAppearing()`, which fixes the refocus defect. Focus model: the filter input is the only focus target; rows are drawn without nav and a virtual highlight index moves with Up/Down/PageUp/PageDown/Home/End and with mouse hover. When the popup is focused and no item is active (after a row click, a wheel scroll or a scrollbar drag), queued printable characters are spliced into the filter buffer with the existing `SpliceTypedCharsIntoBuf` and focus returns to the input. Enter picks the highlighted row; Esc closes. The list is clipped with `ImGuiListClipper`. Row decisions (filter match, match span, visible-row model with Recent/All sections, highlight movement, create-row eligibility) live in `FilterDropdownPure.h` and are unit-tested without ImGui.

Each picker in scope builds a small `Item` array from the options it already has, passes a recents scope and handles the returned event (picked / cleared / created). The widget records recents itself, so no picker carries its own copy. The labels editor's separate "Add label" input becomes an `Add "<text>"` row shown when the filter has no exact match.

## Interface contracts

- **No change** to `ITrackerBackend` or its role interfaces, commands, Lua bindings, MCP schemas, view storage or the SQLite cache.
- **Config format (additive)**: new top-level key `recent_selections` in `smatchet_config.json`: an array, most recently touched scope first, of `{ "scope": "<scopeKey>", "ids": ["<id>", …] }`, at most 5 ids per scope and at most 128 scopes. Unknown to older builds, which preserve unmodelled keys through `ConfigManager::UpdateConfigJson`. A malformed value loads as empty (logged with `LOG_WARN`).
- **New API** `Source/Core/include/RecentSelections.h`: `smatchet::recents::RecentSelectionStore` (`Record`, `Get`, `ToJson`, `FromJson`, `Clear`), `smatchet::recents::SharedGet(scope)` (a copy of the process instance's list), `smatchet::recents::RecordAndPersist(scope, id)`, `smatchet::recents::LoadSharedFromConfigJson(json)`, `smatchet::recents::ResetSharedForTests()`, `smatchet::recents::BeginSharedTestScope()` / `EndSharedTestScope()` (a UI-test run inside a live session gets its own empty store and the session's picks back afterwards), `smatchet::recents::MakeScopeKey(backendKey, fieldId)`.
- **New API** `Source/Core/include/FilterDropdown.h`: `smatchet::dropdown::Item`, `Options`, `Event`, `DrawBody(const Item*, int, const Options&)`, `DrawBody(const std::vector<Item>&, const Options&)`, and the option presets `SingleSelect(recentsScope)` and `TickBoxes(recentsScope, clearAllLabel)`.
- **New API** `Source/Core/include/TrackerOptionDropdown.h`: `smatchet::dropdown::OptionPickId(option)` and `ItemForOption(option)`, the one place a `TrackerFieldOption` becomes a dropdown row.
- **Plumbing**: picker → `DrawBody(items, SingleSelect(MakeScopeKey(app.FocusedCacheBackendKey(), field.Id)))` → on pick `RecordAndPersist(scope, id)` → the process store's `Record` + `EnqueueConfigJsonKey("recent_selections", <store JSON>)` → config-save worker → `ConfigManager::UpdateConfigJson`. Load: `AppController` init → `ConfigManager::LoadMergedConfigJson()` → `LoadSharedFromConfigJson`. Persistence stays off until that load has run.

## Files to modify

New:

1. `Source/Core/include/RecentSelections.h`, `Source/Core/src/RecentSelections.cpp` — the store, shared instance and persistence glue.
2. `Source/Core/include/FilterDropdownPure.h` — ImGui-free row model, filter match, highlight movement.
3. `Source/Core/include/FilterDropdown.h`, `Source/Core/src/FilterDropdown.cpp` — the ImGui body.
4. `tests/Core/RecentSelections.test.cpp`, `tests/Core/FilterDropdownPure.test.cpp` — doctest; listed in `tests/CMakeLists.txt`.
5. `tests/ui/filter_dropdown.test.cpp` — bucket E; listed in `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp`.

Wired:

6. `Source/Core/src/TicketFieldEditor.cpp` — `RenderSingleSelectComboBody` and `RenderMultiSelectComboBody` draw through `DrawBody`.
7. `Source/Core/src/Tracker/TrackerLabelsEditor.cpp` — `DrawLabelsComboBody` draws through `DrawBody` with the create row.
8. `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` — `DrawDraftOptionCombo`.
9. `Source/Core/src/Ui/SmatchetProjectPicker.cpp`, `Source/Core/include/Ui/SmatchetProjectPicker.h` — one list (all projects plus cached-catalog projects), recents from the shared store, list load kicked on open.
10. `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp` — `DrawJqlProjectPill` popup body.
11. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp` — `##ViewSelectorCombo` body.
12. `Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp` — `##quickcreate_type` body.
13. `Source/Core/src/AppController_Init.cpp` — load the shared store from config.
14. `Source/Core/include/SpreadsheetState.h`, `Source/Core/include/Ui/SmatchetUiSession.h` — drop the per-picker search buffers the shared state replaces, when nothing else reads them.
15. `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`.

Core `.cpp` files are globbed (`Source/Core/CMakeLists.txt`), so no list edit there.

## Existing utilities reused

- `TicketFieldEditorDurationPopupPure::IsPrintableTypedChar` / `SpliceTypedCharsIntoBuf` (`Source/Core/include/TicketFieldEditorDurationPopupPure.h`) — the typed-character splice; reused as is.
- `smatchet::config_save::EnqueueConfigJsonKey` (`Source/Core/include/ConfigSaveWorker.h`) — async coalescing config write.
- `SmatchetTouchEdit::ArmThenPopupCellGate` (`Source/Core/include/TouchCellEditGesture.h`) — unchanged; the body is drawn inside the combo it opens.
- `AppController::FocusedCacheBackendKey` (`Source/Core/include/AppController.h`) — backend part of the scope key.
- `SmatchetLocalization::T` — every new user-visible string.
- `ToLowerAsciiCopy` / `TrimCopy` (`Source/Core/include/StringUtil.h`).

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted from an over-cap file; the picker bodies shrink because their loops move into the shared body.

## UX Pillar callouts

- **Pillar 1 (perf)**: the body runs only while a popup is open; rows are clipped; the filter match is allocation-free per row. The collapsed-cell hot path is untouched.
- **Pillar 2 (UI never freezes)**: a pick updates memory and enqueues an async config write; load happens once at init from JSON that is already parsed. No new sync I/O on the frame thread. The project picker's list load stays on the background-task pool.
- **Pillar 3 (never crash)**: malformed persisted JSON loads as empty; ids and scope keys are length-capped; the splice is bounds-checked.
- **Pillar 4 (accessibility)**: full keyboard operation (type, Up/Down, PageUp/PageDown, Home/End, Enter, Esc); sizes derive from font size; colours come from the active ImGui style.
- **Pillar 5 (DRY)**: one store and one body replace six hand-rolled copies.
- **Pillar 6 (offline-first)**: recents render from memory with no network call; the project picker shows recents and cached-catalog projects before, or without, the live listing; existing freshness cues stay above the body.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires: `cell-edit-burst` and `priority-grid-scroll` (grid render and edit), both in the PR-fast set. The collapsed-cell path is unchanged; the open-popup path is new code.
2. **Pillar 2 static scanner** — N/A: no new sync I/O reachable from `ImGui::*`; persistence goes through `EnqueueConfigJsonKey`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new sync-stall path.
5. **Marker inventory** — N/A: no new `SMATCHET_UI_PERF_SCOPE` markers.

## Risks / non-goals

- **Config growth** — bounded: 128 scopes × 5 ids, ids capped at 256 bytes.
- **Focus stealing** — the refocus rule applies only while the dropdown's own popup window is focused and no item is active, so sibling inputs in a parent window are unaffected. Covered by bucket E.
- **Esc semantics** — Esc closes the popup in one press (the input no longer swallows the first Esc).
- **Labels create flow changes** — the separate "Add label" input is replaced by an `Add "<text>"` row. Accepted as part of the redesign.
- **Project picker loads its list on open** instead of on first expand of a collapsible section. The load is unchanged (background pool, saved-copy first, once per picker state).
- **Dual target** — both new units depend only on ImGui and Core headers; no GLFW/OpenGL includes.
- **Non-goals**: settings/preferences combos; cascading select; time-of-day and duration popups; template popups; JQL autocomplete; command palette and "Recently Used Views" (they keep their own MRUs).

## Implementation handoff

N/A — single slice, orchestrator. Order: store and pure model with tests → ImGui body → wire pickers → bucket E → gates.

## Verification

- **Bucket A (doctest)**: `tests/Core/RecentSelections.test.cpp` — cap at 5, newest-first, dedup-and-promote, per-scope isolation, scope-count eviction, JSON round-trip, malformed JSON. `tests/Core/FilterDropdownPure.test.cpp` — filter match and match span, Recent section threshold (> 10 options), recents hidden while filtering, recents absent from the list hidden, checked-stays-visible, create-row eligibility, highlight movement and clamping.
- **Bucket E (ImGui Test Engine)**: `tests/ui/filter_dropdown.test.cpp` — replica window hosting `DrawBody`: typed characters reach the filter after a row click; Up/Down + Enter picks; the Recent section appears after picks and hides while filtering; Esc closes.
- **Bash-driver scenario / screenshot / sanitizer**: `cell-edit-burst` and `priority-grid-scroll` perf compare.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the eight scope decisions above were settled with the user before this plan was written; an adversarial design review of the plan runs before implementation is final, outcome recorded in § Verification (actual).
- **Manual residue**: the visual look of the redesign inside the real grid is covered by the replica-window bucket-E test for behaviour only; the look itself falls under the visual-validation exception (launch the exe, user verifies).

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Settings combos (~40 sites) — follow-up only if wanted; recents add nothing on 2–10 value enums.
- `CommandRegistry::SaveRecents` does sync file I/O on the UI thread (`Source/Core/src/Commands/CommandRegistry.cpp`) — existing Pillar-2 debt, separate fix.
- Folding `RecentViewLru` and the command-palette recents into the shared store — follow-up.
- Ranking recent users first in JQL autocomplete — follow-up.
- A filter for the Annotate Jira-field combos (large list, no filter) — follow-up.

## Implementation log

- `aca1170ed` · wip(plan): this plan.
- `46e61e30b` · feat(ui): the recents store, the shared dropdown body and its ImGui-free row model, every in-scope picker wired to them, doctest and bucket-E tests, the mutation corpus retarget.

## Deviations from plan

- **Home / End are not highlight keys.** The filter field keeps keyboard focus, and there Home / End move the text caret. The highlight moves with Up / Down / PageUp / PageDown and the mouse. § Approach and § UX Pillar callouts (Pillar 4) still list Home / End: read them without.
- **Config value is an array, not an object** (§ Interface contracts is corrected in place): an array keeps the scope order, which is what the 128-scope eviction needs.
- **Store API**: `Shared()` is not exposed. Callers get `SharedGet(scope)` (a copy) and `RecordAndPersist`; tests get `ResetSharedForTests()`. § Interface contracts is corrected in place.
- **Load reads the config through `ConfigManager::LoadMergedConfigJson()`** at `AppController` init, before the first frame; § Approach says "already-parsed JSON".
- **Typed characters are spliced whenever the filter field does not hold keyboard focus** while the popup is focused, which includes a scrollbar drag in progress (a mouse button is down, so focus cannot return yet). § Approach says "no item is active".
- **No "match span" decision in the pure header.** The match position comes from `FindCaseInsensitive`; the highlight box is drawn by the ImGui body.
- **`ToLowerAsciiCopy` / `TrimCopy` are not used**; the pure header has its own allocation-free ASCII compare and `TrimFilter`.
- **Added after code review** (four-agent adversarial review of the first complete build):
  - Enter under a filter acts on the first option that matches it, never on a ticked option that `KeepCheckedVisible` only keeps listed (`DefaultHighlight`).
  - The clear row is never the default highlight, so Enter in a picker whose options have not arrived does not clear the field.
  - The empty-state text and hint are drawn whenever the list holds no option or create row, also under a lone clear row.
  - The highlight follows its row by identity (id and label) when the option list is rebuilt under an open popup, and takes a fresh default when the list changes size before the user has moved it. An arrow key with no row to move to does not count as a move; a scrollbar drag does.
  - Under a filter, Enter acts on the best match: a label that starts with the filter, then a label that contains it, then an id or secondary-text match (`MatchRank`). The list order is unchanged.
  - A row whose secondary text does not fit shows it in the tooltip.
- **Labels: Enter on a partial name picks the first match, not a new label.** With "ui-polish" existing, typing "ui" and Enter ticks "ui-polish"; the `Add "ui"` row is one arrow key away. The old separate "Add label" input added exactly the typed text.
- **Deferred to the debt backlog** (second review round, both Low): cell pickers key recents by the focused pane's tracker (`docs/self-improvement/categories/debt/2026-11-13-dropdown-recents-scope-uses-focused-backend.md`); the project picker rebuilds its rows every frame the popup is open (`docs/self-improvement/categories/debt/2026-11-13-project-picker-rebuilds-rows-every-open-frame.md`). Not fixed, cosmetic: moving the mouse onto a row below the highlight draws two highlighted rows for one frame.
- **Added after the gate review** (`code-review` agent on the complete diff):
  - The Enter that opens a keyboard-focused combo only opens it. The popup appears in the same frame with the key still down, so the body ignores Enter until that press is released; before, it picked the default row and closed the popup at once (project picker, quick-create type, draft combos, view selector, project pill).
  - A space typed into an empty filter is dropped, so the Space that opens a combo is not filter text.
  - **The project picker's Recent section starts from the saved catalogs.** Before any pick is recorded for a tracker, the section is filled once from the projects whose field catalog is saved on this machine, most recently used first (`RecentSeedKeys`), taking only projects the focused tracker's own list holds and never without a tracker. Without it, the first run of this build would have moved a user's daily projects from the top of the popup into the full list. The dropdown reads its recents when it opens, so the section shows from the second open.
  - `RecentSelections.cpp` and its test are also built into the `SmatchetTsanTests` subset, the mutation-smoke oracle, so the `RECENTS-01-cap` mutant can be killed.
  - The UI tests take the recents store through `BeginSharedTestScope` / `EndSharedTestScope` (the test's `TeardownFunc`), so a run inside a live session neither wipes its picks nor leaves saving off.
  - Left open, Low: no bucket-E case for the leading-space skip; the seed's glue (once per scope, skipped when the scope has picks) has no test of its own, only `RecentSeedKeys` does.
  - Not changed: the splice does not check for another active text field in the popup (no caller has one); the tables in `docs/audits/MUTATION_PILOT.md` are the dated record of the pilot run, so only the retired mutant's entry carries a note.
- **Added for the DRY gate**: `DrawBody(vector)`, `SingleSelect`, `TickBoxes` and `TrackerOptionDropdown.h` (`ItemForOption`), so the seven call sites share their set-up instead of repeating it.
- **Files beyond § Files to modify**:
  - Deleted: `Source/Core/include/TicketFieldEditorOptionFilterPure.h` and `tests/Core/TicketFieldEditorOptionFilterPure.test.cpp` (the per-editor option filter the shared row model replaces).
  - Trimmed: `Source/Core/include/Tracker/TrackerLabelsPure.h`, `Source/Core/src/Tracker/TrackerLabelsPure.cpp`, `tests/Core/TrackerLabelsPure.test.cpp` (`LabelMatchesFilter` and the add-label helpers); `Source/Core/include/Ui/SmatchetNewIssueDraftUi_detail.h`, `Source/Core/src/Ui/SmatchetNewIssueDraftUi_detail.cpp`, `tests/Core/NewIssueDraftFieldDecision.test.cpp` (the draft combo's own filter).
  - Added: `Source/Core/include/TrackerOptionDropdown.h`, `tests/Core/TrackerOptionDropdown.test.cpp`, `scripts/dev/test-ui-filter-dropdown.sh` (bucket-E runner), the two debt entries named above.
  - Edited: `Source/Core/src/SmatchetLocalization.cpp` (`dropdown.*` strings), `Source/Core/include/Ui/SmatchetProjectPicker_detail.h`, `Source/Core/src/Ui/SmatchetProjectPicker_detail.cpp` and `tests/Core/ProjectPickerFilter.test.cpp` (`RecentSeedKeys`), `scripts/dev/mutation-smoke-corpus.json` and `docs/audits/MUTATION_PILOT.md` (the `LabelMatchesFilter` mutants: one retargeted to `LabelsEqualCaseInsensitive`, the equivalent one retired; a new `RECENTS-01-cap` guard).
- **Project picker**: `StartRecentProjectsLoad` is renamed `StartCachedProjectsLoad` (its rows are no longer the "recent" ones), and `StartAllProjectsFetch` runs when the combo opens.
- **Quick-create issue type** rows now show an option's secondary text when it has one, like every other option picker (`ItemForOption`).
- **Advisory lint, not blocking**: `tu-line-ceiling` on `SmatchetLocalization.cpp` and `TicketFieldEditor.cpp` (both over 1,200 lines before this change), the soft function-size tier on `DrawHeaderViewToolbar` (103 lines).

## Verification (actual)

- **Bucket A**: `SmatchetTests` (ninja-test-msvc) full suite 4,121 / 4,121 passed. New: `RecentSelections.test.cpp` (14 cases), `FilterDropdownPure.test.cpp` (row model, default highlight incl. the kept-ticked, clear-row and match-rank cases, row identity, highlight moves, scroll maths), `TrackerOptionDropdown.test.cpp` (the Id-or-Value pick id); `ProjectPickerFilter.test.cpp` gains `RecentSeedKeys`.
- **Bucket E**: `scripts/dev/test-ui-filter-dropdown.sh` 16 / 16 passed (cases A–P): type on open, arrows + Enter, typing after a row click, after a wheel scroll and during a scrollbar drag, Recent section pinned then hidden while filtering, Escape, untick records nothing, the create row, Enter on the match with a kept ticked option, a recent pick outside the options, PageDown, the empty state under a lone clear row, options arriving under an open popup, rows inserted above the highlight, the Enter that opens the combo. Case P was seen failing before the fix. The splice was mutation-checked by hand: with it disabled, the scrollbar-drag case fails.
- **Build gate**: `SmatchetStandalone` and `SmatchetCore_DX12` build clean on ninja-iter-msvc; the UI-test exe builds on ninja-ui-test-msvc.
- **Lint**: `test-lint-rules.sh --diff origin/develop` passes; the first run failed on `function-too-branchy` (two functions), `duplication` (five call-site clones) and `comment-commented-out-code` (two comments), all fixed.
- **Look**: checked on screenshots of the replica window (match highlight aligned with the text, focus ring around the whole filter row). The look inside the real grid is the user's to confirm (visual-validation exception).
- **Code review**: four-agent adversarial review of the first complete build (store, widget, wiring, tests and gates), findings fixed (§ Deviations); `code-review` agent on the fixes (0 Critical / High, 3 Medium fixed, 7 Low: 3 fixed, 1 accepted as a design choice, 2 to the debt backlog, 1 cosmetic left); `code-review` gate review of the complete diff, three rounds. Round 1: 2 High (the Enter that opens the popup also picked a row; the new mutant had no oracle), 2 Medium, 5 Low. Round 2, on the fixes: 1 Medium (the Recent seed could take another tracker's projects), 3 Low. Round 3: 0 Critical, 0 High; the visual check stays owed (Medium, the visual-validation exception) and 4 Low are left open or recorded here and in the debt backlog.
- **Not run**: the `SmatchetTsanTests` subset (Linux only; CI builds it); `cell-edit-burst` / `priority-grid-scroll` perf compare locally (left to the PR-fast CI lane); `grill-with-docs` as a separate pass (the eight scope decisions were settled with the user before the plan; the review above read this plan against the code); the Unreal editor itself (only `SmatchetCore_DX12` is compiled here).
