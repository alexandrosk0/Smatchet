# Plan — Shared filter dropdown with recent picks

> **Slug**: `shared-dropdown-recents` (matches this file's basename without `.md`).
>
> **Status**: `active`

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
- **Config format (additive)**: new top-level key `recent_selections` in `smatchet_config.json`: `{ "<scopeKey>": ["<id>", …] }`, at most 5 ids per scope and at most 128 scopes. Unknown to older builds, which preserve unmodelled keys through `ConfigManager::UpdateConfigJson`. A malformed value loads as empty (logged with `LOG_WARN`).
- **New API** `Source/Core/include/RecentSelections.h`: `smatchet::recents::RecentSelectionStore` (`Record`, `Get`, `ToJson`, `FromJson`, `Clear`), `smatchet::recents::Shared()` (process instance), `smatchet::recents::RecordAndPersist(scope, id)`, `smatchet::recents::LoadSharedFromConfigJson(json)`, `smatchet::recents::MakeScopeKey(backendKey, fieldId)`.
- **New API** `Source/Core/include/FilterDropdown.h`: `smatchet::dropdown::Item`, `Options`, `Event`, `DrawBody(const Item*, int, const Options&)`.
- **Plumbing**: picker → `DrawBody(items, opts{RecentsScope = MakeScopeKey(app.FocusedCacheBackendKey(), field.Id)})` → on pick `RecordAndPersist(scope, id)` → `Shared().Record` + `EnqueueConfigJsonKey("recent_selections", Shared().ToJson())` → config-save worker → `ConfigManager::UpdateConfigJson`. Load: `AppController` init → `ConfigManager` merged JSON → `LoadSharedFromConfigJson`.

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
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
