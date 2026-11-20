# Plan — input UX Wave 3: the grid as a spreadsheet

> **Slug**: `input-ux-w3-grid-spreadsheet` (matches this file's basename without `.md`).
>
> **Status**: `active` — plan only; drives four grid PRs (S1 → S4) against `develop`.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

[`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 5.3 lists "grid as a spreadsheet" as one Wave 3 structural item: active cell, type-to-edit, Enter-down and Tab-right, cell ranges, paste, bulk apply, undo (**#26**, **#27**, **#28**, #29, #30). § 7.3 has the evidence. Today a value cell is a `Selectable`. A click (or a double click, by preference) opens the inline editor. Enter commits and nothing moves on. Arrows only drive ImGui's generic nav. A plain click on a value cell clears every selection, so a cell range can never be started (`SmatchetActiveProjectGridCells.cpp:599-602`). Nothing in the grid handles F2, typing, Tab, Ctrl+V or Ctrl+Z.

Two sibling pieces already cover part of this, and this plan builds on them instead of redoing them:

- **Bulk edit P1** ([`docs/plans/bulk-edit-rows.md`](../bulk-edit-rows.md), shipped in #2432 / #2434; finish-up in #2450) added "Set <Field> for N rows…" on the cell context menu. It also added the bulk job core (`BulkFieldEditPure.h`, `PrepareBulkFieldEditJob` / `StartBulkFieldEdit` in `SmatchetGridFieldEditPipeline.cpp:506,576`) and selection kept by issue id while a job runs (`GridRectSelection::KeepIssueIds`, `SpreadsheetState.h:37-64`). That closes the context-menu half of #27. Bulk P2 (Revert, text / date / number in the popup) and P3 (a parallel pump) are still ahead.
- **Wave 2 piece 4.3 drafts and undo** (#2439, open; its plan `input-ux-w2-drafts-undo` lands with it) adds `SmatchetToastManager::PushUndoable` and a Ctrl+Z that undoes the newest live undo toast (`Ui/UndoLedgerPure.h`). It deliberately leaves grid edit undo (#28, #31, #33) to this wave.

After this lands, the Active Project grid works like a spreadsheet. A click or the arrow keys move an **active cell**. Shift extends a **cell range**. F2, Enter or typing starts an edit. Enter commits and moves down, Tab commits and moves right, and Ctrl+Enter commits the value to every selected row. Ctrl+V pastes a value or a TSV block, and Ctrl+D fills down. Every committed edit can be undone with Ctrl+Z or the Undo button on its toast. A rejected value comes back in the editor instead of being lost.

Closes (audit § 7.3): **#26**, **#27** (the rest: Ctrl+Enter, fill-down, paste over a range), **#28**, **#29**, **#30**. The design closes these too: #31 (a rejected value is kept), #32 (selection survives a sync, since the active cell has to), #33 (a blur-commit becomes undoable) and #34 (the per-edit success toast becomes the undo toast). Left for later: #35 and #36 (§ Out of scope).

## Approach

**The active cell is stored by identity and drawn by index.** `SpreadsheetState` already has `ActiveIssueId`. It gains `ActiveFieldId`, so the active cell is an (issue id, field id) pair. That pair survives a re-sort, a sync and a landed edit, all of which bump `snapshotRevision` and clear the index-based selection today (`SmatchetActiveProjectGridTable.cpp:1167-1173`). The grid maps the pair to a (row, column) pair of ints **once per sort-signature change**, not once per cell. The per-cell code compares ints only, which keeps `Source/Core/src/Ui/AGENTS.md` § Per-cell grid code allocates nothing per row. Selection rows and range corners get the same identity mirror. The bulk-only `KeepIssueIds` / `KeepRows` / `KeepStale` mechanism becomes the general one: a selection is remapped to the new order and only missing rows drop out (#32). The movement math (arrows, Home / End, Ctrl+Home / End, PageUp / PageDown, Tab wrap, skip read-only on commit-and-move) is a pure header, `GridCursorPure.h`, tested in bucket A.

**Select and edit become separate gestures.** A plain click on a value cell makes it the active cell and starts a one-cell range. It no longer clears to nothing. A drag extends the range, and Shift+click / Shift+arrows extend it from the anchor. The editor opens on F2, on Enter, on a typed character (type-to-edit replaces the value, as in Sheets), on double-click, or, when *Single-click to edit grid cells* is on (the default), on a click that is released on the same cell. A drag that ends on another cell never opens an editor, so the preference keeps its meaning for click-to-edit users and ranges still become possible. Keys are claimed with ImGui's public key-ownership API (`SetKeyOwner` / `Shortcut` with `ImGuiInputFlags_RouteFocused`, `SetItemKeyOwner(ImGuiKey_Tab)` inside the editor; precedent `SmatchetAiAssistantUi.cpp:1226`), so ImGui's generic nav does not move a second cursor. The clipper keeps the active row drawn (`ImGuiListClipper::IncludeItemByIndex`), and the active cell scrolls into view on the frame it moves.

**Every write still goes through the one edit pipeline, and undo is the inverse edit.** Commit-and-move, type-to-edit and a single-cell paste hand a `PendingFieldEdit` to `QueueEdit` (`TicketFieldEditor.cpp:377`), as every grid editor does today. Ctrl+Enter, Ctrl+D and a range paste build a bulk job, so N rows get one summary toast, per-row badges and the bulk skip of non-editable rows. They go through a new `RequestBulkFieldEdit`, which asks before writing more than 10 rows: `StartBulkFieldEdit` itself never confirms (today only the context-menu popup does). A small pure undo history (`GridUndoPure.h`, bounded, per session) records {pane, issue, field, before-values, after-values} where the pipeline enqueues an edit (`EnqueueGridFieldEdits`) and per bulk job. Undo queues the inverse as an ordinary edit whose ADR-0016 scalar base is the value we wrote. If someone else changed the field meanwhile, the existing conflict path reports it, and nothing is silently overwritten. The undo toast is #2439's `PushUndoable`: it replaces the per-edit "Field update saved to Tracker." success toast (#34). Its Undo button and Ctrl+Z undo that one entry, and the entry stays in the grid's history after the toast leaves. Ctrl+Z goes to the newest live undo toast first, which is #2439's contract. With no live undo toast and the grid focused, it pops the grid history. A rejected commit keeps the typed value in `CellWriteFeedback`, and reopening the cell (or the "!" badge) puts it back in the editor with the message under it (#31).

## Interface contracts

No tracker interface, MCP schema, Lua binding, config key, cache or SQLite format changes. Two new pure headers and in-memory struct widenings:

```cpp
// Source/Core/include/Ui/GridCursorPure.h — header-only, no ImGui, C++14
namespace smatchet { namespace grid { namespace cursor {
struct CellPos { int Row = -1; int Col = -1; bool Valid() const { return Row >= 0 && Col >= 0; } };
enum class Move { Up, Down, Left, Right, Home, End, Top, Bottom, PageUp, PageDown, TabNext, TabPrev };
// Clamped step inside [0,rowCount) x [0,colCount). TabNext / TabPrev wrap to the next / previous row at the edges.
CellPos Step(CellPos from, Move m, int rowCount, int colCount, int pageRows);
// Commit-and-move target: steps like Step, then skips cells `editable(row, col)` rejects, up to one full sweep;
// returns `from` when nothing editable lies that way.
template <class Editable> CellPos StepToEditable(CellPos from, Move m, int rowCount, int colCount, Editable editable);
struct Range { CellPos Anchor; CellPos Extent; int MinRow() const; int MaxRow() const; int MinCol() const; int MaxCol() const; };
Range Extend(const Range& r, Move m, int rowCount, int colCount, int pageRows);   // Shift+arrow
// A range's membership as GridRectSelection::Remap builds it: one byte per shown row / column, nonzero = member.
// Passed instead of two corners, so a range that a re-sort split (rows A, B shown as A, C, B) never fills C.
struct RangeMask { const std::vector<char>* RowIn = nullptr; const std::vector<char>* ColIn = nullptr; };
// A TSV block (CopyGridRectAsTsv's format) laid onto the grid at `at`. One value with a `fill` mask fills every
// member cell (RowIn[r] && ColIn[c]) and nothing else; one value without a mask fills `at`. A block is placed
// from `at` over the shown cells and clipped to the grid; the mask does not shape it. Pure; rows/cols outside
// the grid are dropped and counted; a mask whose sizes differ from rowCount / colCount fills nothing.
struct PasteCell { int Row; int Col; std::string Text; };
struct PastePlan { std::vector<PasteCell> Cells; int Dropped = 0; };
PastePlan PlanPaste(const std::string& tsv, CellPos at, const RangeMask* fill, int rowCount, int colCount);
}}}

// Source/Core/include/Ui/GridUndoPure.h — header-only, no ImGui, C++14
namespace smatchet { namespace grid { namespace undo {
enum class ChangeState { Pending, Landed, Failed };   // Landed = saved online or queued offline
struct CellChange { std::string IssueId; std::string FieldId; std::vector<std::string> Before; std::vector<std::string> After;
                    std::string BeforeDisplay; std::string AfterDisplay;     // AfterDisplay is valid only when Landed
                    ChangeState State = ChangeState::Pending; };             // (an empty AfterDisplay is a cleared field)
struct Entry { std::uint64_t Id = 0; std::string PaneId; std::vector<CellChange> Changes; std::uint64_t BulkJobId = 0; };
constexpr std::size_t kMaxEntries = 50;   // oldest dropped past this; in memory only
class History {
  public:
    std::uint64_t Push(Entry e);               // returns the entry id
    // A commit result for one change of entry `id` (UI thread). Landed records the display value after the local
    // apply. Failed drops that change, and an entry left with no change is removed. Unknown id: no-op, false.
    bool MarkLanded(std::uint64_t id, const std::string& issueId, const std::string& fieldId, const std::string& afterDisplay);
    bool MarkFailed(std::uint64_t id, const std::string& issueId, const std::string& fieldId);
    bool Take(std::uint64_t id, Entry& out);   // removes one entry (toast Undo)
    // Grid Ctrl+Z: the pane's newest entry. One that still has a Pending change is NOT taken; the result says so,
    // so the caller can show "still saving" and keep it (an undo issued before the save lands would invert nothing).
    enum class TakeResult { Taken, StillSaving, Empty };
    TakeResult TakeNewest(const std::string& paneId, Entry& out);
    std::size_t Size() const;
};
// The inverse edits of an entry, newest change first: writes Before, with AfterDisplay as the ADR-0016 conflict base.
// Only Landed changes are inverted.
std::vector<CellChange> Inverse(const Entry& e);
}}}
```

Widened in-memory types:

- **`SpreadsheetState`** (`Source/Core/include/SpreadsheetState.h:89`): `std::string ActiveFieldId;`. Derived caches `int ActiveRow = -1, ActiveCol = -1;` are rebuilt on a sort-signature change. `bool ScrollActiveIntoView = false;` is set on a keyboard move. `StartEditingFieldWithText(id, fieldId, originalValue, seed)` is for type-to-edit: the buffer holds `seed`, and the dirty baseline stays `originalValue` truncated as today, so an unchanged commit never PUTs. `char EditBuffer[512]` stays (#36 is out of scope).
- **`GridRectSelection`** (`SpreadsheetState.h:19`): `Rows` gains `RowIssueIds`. A cell range is a **membership fixed when it is made**, not two corners. Each gesture that sets or extends the range (click, drag, Shift+click, Shift+arrow) snapshots `RangeIssueIds` (the issue ids of the rows from anchor to extent, in the order they were shown) and `RangeFieldIds` (the columns from anchor to extent). After a re-sort, the range is the same cells wherever they now sit: a row that moved out of the block stays in, a row that moved between two selected rows does NOT join, and a row that left the view drops out. Corners would add that row (rows A, B sorting to A, C, B makes corners 0 and 2, which includes C), so they are kept only as the anchor and extent identities that Shift+arrow extends from. For the per-cell test, `Remap` builds `std::vector<char> RowInRange` and `ColInRange` (one byte per shown row / column) once per sort-signature change. `RectContains(r, c)` reads two bytes and allocates nothing. A range that is no longer contiguous is drawn cell by cell from that test, and Ctrl+Enter / Ctrl+D / Ctrl+C act on its members only. `RestoreKeptRows` generalises to `Remap(rowCount, idAt, colAt, sortSignature)`, and the clear at `SmatchetActiveProjectGridTable.cpp:1171-1173` becomes a remap. `KeepIssueIds / KeepRows / KeepStale` are folded into the always-on mirror (bulk D5 keeps its behaviour: the rows of a running job stay selected across each landed row).
- **`CellWriteFeedback`** (`Source/Core/include/Ui/SmatchetUiSession.h:239`): `std::string RejectedValue;` is set on `CellWriteState::Error` from the edit's values (#31).
- **`UiDrawSession`**: `smatchet::grid::undo::History gridUndo;`.
- **`PendingFieldEdit`** (`SmatchetUiSession.h:116`): `bool IsUndo = false;` and `std::uint64_t UndoEntryId = 0;`. An undo's own commit does not push a new history entry or a second undo toast, and its toast reads "Undone". `UndoEntryId` is stamped when the entry is pushed, so the commit result finds its entry (0 = not recorded). Neither field is persisted to the offline queue.
- **Per-row bulk values** (`Source/Core/include/Ui/SmatchetGridUiSupport.h:147,155`, `BulkFieldEditPure.h`). Today `PrepareBulkFieldEditJob` / `StartBulkFieldEdit` take one shared `values` vector for every row, which cannot express a pasted column with different values per row, or an undo that restores A on one row and B on another. Both functions gain an overload that takes `const std::vector<smatchet::grid::bulk::BulkRowInput>& rows`, where `struct BulkRowInput { std::string IssueId; std::vector<std::string> Values; };` is new in `BulkFieldEditPure.h`. The mode is always Replace, and each row's `Values` are its own. The existing shared-value form becomes a thin wrapper that builds the per-row input with the same values on every row, so the context-menu popup and `ticket.set_field_bulk` keep their behaviour. The per-row skip ("already holds this value", not editable, no longer shown) is unchanged. Fill-down and Ctrl+Enter use the shared form. A block paste and every undo of a bulk job use the per-row form, the undo built from the entry's per-row `Before`.
- **Bulk confirm** (new, `Source/Core/include/Ui/SmatchetGridUiSupport.h`): `RequestBulkFieldEdit(app, d, paneId, field, rows, readOnly)`, with the same shared-value and per-row overloads as `StartBulkFieldEdit`. It runs the `PrepareBulkFieldEditJob` preview, and when `NeedsConfirm(rows)` holds (above 10 rows, `BulkFieldEditPure.h:100`), it parks the request on `UiDrawSession::pendingBulkConfirm` and draws "Apply to N rows? K will be skipped" through the Wave 2 dialog contract (`ConfirmFooter`, `Ui/SmatchetDialogScope.h:107`). Only Apply calls `StartBulkFieldEdit`. Today that confirm exists only in the context-menu popup (`SmatchetBulkFieldEditPopup.cpp:207`), and `StartBulkFieldEdit` fans out at once, so every keyboard bulk write goes through this request instead.

Plumbing, hop by hop:

- **Commit-and-move.** `RenderTextInlineEdit` returns an `InlineEditOutcome {Committed, Cancelled, None; Move}` instead of `void`. `RenderTextEditor` passes it up, and `TicketFieldEditor::RenderFieldCell` gains an `InlineEditOutcome* outOutcome = nullptr`. The value-cell draw in `drawActiveProjectGridValueCell` stores it on `ActiveProjectDrawCtx::pendingMove`. `drawActiveProjectGridCursorKeys` consumes it after the row loop and moves the active cell. Combo / user / date editors report `Committed` with `Move::Down` when their popup closes on Enter.
- **Bulk apply.** `Ctrl+Enter` in the editor, `Ctrl+D`, and a paste over a range call `RequestBulkFieldEdit`, once per affected column: Ctrl+Enter and Ctrl+D with the shared value, a block paste with the per-row form (each row's own pasted value). A multi-column paste confirms once for all its columns: the dialog counts the total of rows × columns. Each started job pushes **one** history entry (`Entry::BulkJobId` = the job id, one `CellChange` per row that is sent). Its rows' `UndoEntryId` point at that entry, and `FoldBulkRowResult` marks each row landed or failed. One Ctrl+Z undoes the whole job.
- **Undo.** `QueueEdit` (`TicketFieldEditor.cpp:377`, file-static) already stamps the ADR-0016 scalar base on the edit (`CaptureScalarBase` → `PendingFieldEdit::OriginalValue` / `HasOriginalValue`), so it needs no change and no access to the session. The pipeline pushes the history entry when the edit is enqueued (`EnqueueGridFieldEdits`, `SmatchetGridFieldEditPipeline.cpp:275`), which is the one choke point every grid write already crosses: `CellChange{Before = BeforeValuesFor(edit.Field, kind, edit.OriginalValue), After = edit.Values, BeforeDisplay = edit.OriginalValue}`, with `Entry::PaneId` taken from the edit's latched `PendingActionTarget`, and the new entry id stamped on `edit.UndoEntryId`. Only edits with `HasOriginalValue`, `!IsUndo` and `BulkJobId == 0` are recorded here; a bulk row belongs to its job's one entry (Bulk apply, above). Rich long-text modal edits are not recorded (§ Risks). `ApplyCommitResultOnUiThread` (`SmatchetGridFieldEditPipeline.cpp:58`) reads `edit.UndoEntryId`. On `SavedOnline` / `QueuedOffline` it calls `MarkLanded` with the ticket's display value after the local apply, which is the inverse edit's conflict base and may be empty for a cleared field. Then, for a single edit, it calls `PushUndoable(title, message, 6000, undo = Take(id) and queue Inverse(entry), commit = no-op)` in place of the success toast. On failure it calls `MarkFailed`. A bulk job pushes its undo toast once, from `PushBulkSummaryToast`, when at least one row landed. Grid Ctrl+Z on an entry that is still saving gets `StillSaving`: the entry is kept and an info toast says "Still saving — try again when it lands".

## Files to modify

**New: pure logic**

1. `Source/Core/include/Ui/GridCursorPure.h`: movement, range extend, paste plan (above).
2. `Source/Core/include/Ui/GridUndoPure.h`: bounded undo history and inverse (above).

**Grid state and draw (owner `grid-engine`, `Source/Core/src/Ui/AGENTS.md` § Grid)**

3. `Source/Core/include/SpreadsheetState.h`: active field id, derived row / col caches, identity mirrors, `Remap`, `StartEditingFieldWithText`.
4. `Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp`: `handleActiveProjectCellRectSel` (`:525`) separates select from edit (#30). The active-cell focus rectangle is drawn next to the selection fill (`:617-621`). `drawActiveProjectGridRectSelKeys` (`:649`) keeps Ctrl+A / Ctrl+C / Esc / Shift+Space and moves the new keys out (item 5).
5. `Source/Core/src/Ui/SmatchetActiveProjectGridKeys.cpp` (new TU, `CORE_SOURCES` glob): `drawActiveProjectGridCursorKeys`. It handles arrows, Home / End, Ctrl+Home / End, PageUp / PageDown, Shift variants, F2 / Enter, type-to-edit from `io.InputQueueCharacters`, Ctrl+V, Ctrl+D and grid Ctrl+Z, and consumes `pendingMove`. All keys are gated on the grid window being focused, `!io.WantTextInput` and no modal. The new TU keeps `SmatchetActiveProjectGridCells.cpp` (710 lines) from growing.
6. `Source/Core/src/Ui/SmatchetActiveProjectGridUi_Internal.h`: `ActiveProjectDrawCtx::pendingMove`, the declaration of `drawActiveProjectGridCursorKeys`.
7. `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp`: the sort-signature clear (`:1167-1173`) and the filtered-count clear (`:1186-1191`) become `Remap`. `RestoreBulkKeptRows` / `DropStaleBulkKeep` (`:818-840`) fold into it. The clipper gets `IncludeItemByIndex(ActiveRow)` before `Step()` (`:1236`). The cursor-keys call goes after the row loop.
8a. `Source/Core/src/TicketFieldEditor.cpp` (S1, the mouse gesture only): `RenderTextEditor` (`:592`) opens the editor through `ShouldOpenCellEditorOnGesture` only on a same-cell release (`:633`), and the combo cells' `ArmThenPopupCellGate` call sites (`:744`, `:931`, `:969`) pass the same input, so a drag that ends on another cell opens nothing.
8. `Source/Core/src/TicketFieldEditor.cpp` (S2, the keys): `RenderTextInlineEdit` (`:437`) reports Enter / Shift+Enter / Tab / Shift+Tab / Ctrl+Enter and claims Tab with `SetItemKeyOwner(ImGuiKey_Tab)`. It reseeds from `RejectedValue` with the message under the field (#31). The combo editors' `ArmThenPopupCellGate` call sites (`:744`, `:931`, `:969`; the gate itself is in `TouchCellEditGesture.h:75`) also open on F2 / Enter / type-to-edit through `EditArmedKey`, and a typed character seeds the dropdown filter.
9. `Source/Core/include/TicketFieldEditor.h`: `InlineEditOutcome`; the `RenderFieldCell` out-param.
10. `Source/Core/include/TouchCellEditGesture.h`: `ShouldOpenCellEditorOnGesture` gains the same-cell-release input; the mobile path (`kMobileInlineEditBuild`) keeps today's behaviour. (#2440, now merged, added one line to this header.)

**Pipeline and toasts**

11. `Source/Core/src/SmatchetGridFieldEditPipeline.cpp`: `EnqueueGridFieldEdits` pushes single-edit undo entries and stamps `UndoEntryId`. `ApplyCommitResultOnUiThread` marks the change landed or failed, swaps the success toast for `PushUndoable`, and fills `RejectedValue`. `StartBulkFieldEdit` pushes the job's one entry. `FoldBulkRowResult` marks rows, and `PushBulkSummaryToast` becomes undoable once a row has landed.
12. `Source/Core/include/Ui/SmatchetUiSession.h`: `CellWriteFeedback::RejectedValue`, `PendingFieldEdit::IsUndo` / `UndoEntryId`, `UiDrawSession::gridUndo` / `pendingBulkConfirm`.
12a. `Source/Core/include/Ui/SmatchetGridUiSupport.h`, `Source/Core/src/Ui/SmatchetGridUiSupport.cpp`: `RequestBulkFieldEdit` and its confirm dialog (above); the per-row overloads of `PrepareBulkFieldEditJob` / `StartBulkFieldEdit` (declared here, defined in `SmatchetGridFieldEditPipeline.cpp`).
12b. `Source/Core/include/Ui/BulkFieldEditPure.h`, `Source/Core/src/Ui/BulkFieldEditPure.cpp`: `BulkRowInput`, and the per-row value plan the job builds from it. The context-menu popup keeps its own confirm step.
13. `Source/Core/src/Ui/SmatchetGridUiSupport.cpp`: the cell context menu gains "Paste", "Fill down (Ctrl+D)" and "Undo <field> on <key> (Ctrl+Z)", each naming its key (Wave 2 piece 4.4 rule).
14. `Source/Core/src/SmatchetLocalization.cpp`: new strings (English fallbacks through `SmatchetLocalization::T`; fr-FR rows when the file is free).

**Tests**

15. `tests/Core/GridCursorPure.test.cpp` (new): every move at the edges, Tab wrap, skip-read-only sweep, range extend, paste plan (one value over a range, one value over a split range: members A and B shown as A, C, B fill A and B but not C, a mask of the wrong size fills nothing, block clip, ragged rows, CRLF, quoted TSV from `CopyGridRectAsTsv`). Registered in both lists in `tests/CMakeLists.txt` (the Linux subset next to `Core/GridCopyPure.test.cpp` at `:186`, and `SmatchetTests` at `:991`).
16. `tests/Core/GridUndoPure.test.cpp` (new): push / take / take-newest per pane, bound, inverse order, bulk entry. Also: `MarkLanded` with an empty display (a cleared field) is still inverted; `MarkFailed` drops the change and removes an emptied entry; `TakeNewest` on an entry with a pending change returns `StillSaving` and keeps it. Same two lists.
17. `tests/Core/BulkFieldEditPure.test.cpp`: the identity remap that replaces `RestoreKeptRows`, with the existing D5 cases kept green; a range that a re-sort splits keeps exactly its members (rows A, B sorting to A, C, B leave C out); per-row input with distinct values in one column, and the shared form equal to per-row input with one value repeated.
18. `tests/ui/grid_spreadsheet_keys.test.cpp` (new, bucket E, fixture lane `test-ui-grid-spreadsheet`): arrows move the active cell and scroll it into view past the clipper, Shift+arrow ranges, F2 / Enter / a typed letter open the editor, Enter-down and Tab-right with read-only skip, Esc reverts and stays, Ctrl+Enter applies to the selection, Ctrl+V single and block, Ctrl+D, Ctrl+Z and the toast Undo restore the value, a rejected value comes back, a sync keeps the active cell and the range. Registered in `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp`.
19. `scripts/dev/test-ui-grid-spreadsheet.sh` (new, modelled on `scripts/dev/test-ui-grid-search-apply.sh`), plus its entry in the grid bucket-E driver loop in `.github/workflows/build-and-test.yml` (`:1891`).
20. `tests/ui/bulk_field_edit.test.cpp`, `tests/ui/duration_inline_edit_commit.test.cpp`: re-pinned where a click no longer clears the selection, and where Enter now moves the active cell.

**Docs**

21. `docs/guides/keyboard-shortcuts.md`: a new "Grid" section (the keys above, and how Ctrl+Z picks between a toast and the grid).
22. `Source/Core/src/Ui/AGENTS.md` § Grid: one bullet. The active cell and the selection are stored by identity and drawn by index, and grid keys claim key ownership, never generic nav.
23. `docs/audits/INPUT_UX_AUDIT.md`: mark the closed findings Done when each slice ships.

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp` | `handleActiveProjectCellRectSel` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp` | `drawActiveProjectGridRectSelKeys` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp` | `RestoreBulkKeptRows` | `CORE_SOURCES` |
| `Source/Core/src/TicketFieldEditor.cpp` | `RenderTextInlineEdit` | `CORE_SOURCES` |
| `Source/Core/include/TouchCellEditGesture.h` | `ArmThenPopupCellGate` | `CORE_SOURCES` |
| `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` | `ApplyCommitResultOnUiThread` | `CORE_SOURCES` |
| `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` | `EnqueueGridFieldEdits` | `CORE_SOURCES` |
| `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` | `StartBulkFieldEdit` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetGridUiSupport.cpp` | `DrawGridCellRightClickPopups` | `CORE_SOURCES` |
| `Source/Core/src/Ui/BulkFieldEditPure.cpp` | `NeedsConfirm` | `CORE_SOURCES` |
| `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` | `PrepareBulkFieldEditJob` | `CORE_SOURCES` |
| `Source/Core/include/TouchCellEditGesture.h` | `ShouldOpenCellEditorOnGesture` | `CORE_SOURCES` |

## Existing utilities reused

- `StartBulkFieldEdit` / `PrepareBulkFieldEditJob`, `Source/Core/src/SmatchetGridFieldEditPipeline.cpp:576,506`: Ctrl+Enter, fill-down and range paste are bulk jobs (skip, summary toast, per-row badges); the confirm is added in front of them by `RequestBulkFieldEdit`, reusing `NeedsConfirm` (`BulkFieldEditPure.h:100`).
- `smatchet::grid::bulk::BeforeValuesFor` / `AllEntriesResolve` / `CaptureScalarBase`, `Source/Core/include/Ui/BulkFieldEditPure.h:105,117,127`: a pasted display string resolves to option ids the way a bulk row's before-value does; an unresolvable entry is skipped and counted, never written as raw text; the undo conflict base.
- `TrackerParseExplain::ParseDurationInput` / `ParseDateInput`, `Source/Core/include/Tracker/TrackerFieldValueParseExplain.h:115,149`: a pasted duration or date is read the tolerant way (Wave 2 piece 4.5), and one that does not read is reported, not written.
- `CopyGridRectAsTsv`, `Source/Core/include/Ui/SmatchetGridUiSupport.h:64`, and `GridCopyPure.h`: the paste format is the copy format, so a copy-paste round trip within the grid is exact.
- `SmatchetToastManager::PushUndoable` / `UndoNewest` (#2439, `Source/Core/include/Ui/SmatchetToast.h`): the undo toast and the shared Ctrl+Z.
- `GridRectSelection::RestoreKeptRows`, `SpreadsheetState.h:51`: generalised into `Remap`.
- `ShouldCommitInlineFieldEdit` / `InlineEditLoadedTruncated` (`TicketFieldEditorCommitPolicyPure`): unchanged commit and truncation guards.
- `ImGui::SetKeyOwner` precedent, `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1226`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A. Nothing is split to clear a cap. The new keys TU (item 5) is new code, so `SmatchetActiveProjectGridCells.cpp` does not grow past its current 710 lines.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: the per-cell cost is two int compares for the active-cell rectangle and the existing `Contains` test, with no allocation. The identity remap is O(rows) and runs only on a sort-signature change (it replaces an O(1) clear, once per landed edit). Paste and fill plan once per key press. Measured on `priority-grid-scroll` before / after each slice (§ Perf gates).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no I/O on the UI thread. Every write goes through the existing worker pipeline. Clipboard reads are ImGui's in-memory `GetClipboardText` (the GLFW clipboard on standalone), once per Ctrl+V.
- **Pillar 3 (never crash)**: every derived index is bounds-checked against `filteredIndices` and `columns` each frame, and a stale or missing identity maps to "no active cell". The paste plan caps rows × columns at the grid size and counts the excess. TSV parsing is linear, with no recursion and no regex. The undo history is bounded.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: this plan is the grid's keyboard path. The focus rectangle uses the theme's `ImGuiCol_NavCursor`, which the palettes in `SmatchetTheme.cpp` set under its old alias `ImGuiCol_NavHighlight` (`:170` and the others; the ImGui-default theme keeps ImGui's own). It is 2 px, so it is not colour-only. Every new action names its key on its menu item.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires. `priority-grid-scroll` (`scripts/dev/perf-pr-fast-set.json`) is the scenario that draws every visible cell each frame. S1 changes the per-cell path, so S1 runs it before and after.
2. **Pillar 2 static scanner**: N/A. No sync I/O is added to a render path.
3. **Dispatcher drain**: N/A. `MainThreadDispatcher::Drain()` is untouched.
4. **Visible-cue bucket-E harness**: N/A. No new stall path.
5. **Marker inventory**: N/A, unless S1 adds a `SMATCHET_UI_PERF_SCOPE("activeProject:grid.cursorKeys")`. If it does, regenerate `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline on `priority-grid-scroll` for S1 (Windows host). The container tier cannot run it, so CI's PR-fast lane is the gate there.

**Override**: none planned.

## Risks / non-goals

- **Behaviour change for pointer users.** A plain click on a value cell now leaves a one-cell range and an active cell, instead of clearing to nothing. A drag between cells now selects instead of doing nothing. Single-click-to-edit keeps opening the editor on a same-cell click. Mitigation: bucket-E pins both preference states, and the change goes in the keyboard guide.
- **Enter-down opens no editor.** The audit's § 7.3 change text says the neighbour's editor opens. This plan moves the active cell and leaves it closed, as Excel and Sheets do, because type-to-edit then replaces the value with no extra key. § Open questions Q1 asks the user to confirm. Tab-right is the same.
- **Key fights with ImGui nav.** Generic nav also moves on arrows and Tab. Mitigation: the grid claims the keys it handles (`SetKeyOwner` / `SetItemKeyOwner`) only while it is focused with no text input. Bucket E asserts that one arrow press moves one cell.
- **Undo after a concurrent change.** The inverse edit's scalar base is the value we wrote. A server change in between takes the ADR-0016 conflict path (badge plus the offline-queue resolver), never a silent overwrite. Multi-value undo restores the before-set (Replace), so an Add someone else made in between is reported, not merged.
- **Long-text (ADF / HTML) edits are not undoable from the grid.** They commit from the modal with `OriginalRichValue`, not a scalar base, and the inverse would have to be a rich document write. Accepted: the modal's kept draft (#2439) covers lost typing, and this plan records scalar edits only.
- **Ctrl+Z has two owners.** The rule is the #2439 contract: the newest live undo toast takes Ctrl+Z first, and the grid history only answers when there is none. A grid edit's own toast is that entry, so the user sees one effect either way. The keyboard guide states it.
- **Overlap with open PRs on the same files.** #2450 (bulk finish: `TicketFieldEditor.cpp`, `SmatchetGridFieldEditPipeline.cpp`, `SmatchetBulkFieldEditPopup.cpp`), #2452 (`SmatchetActiveProjectGridCells.cpp`, `SmatchetActiveProjectGridTable.cpp`, `TicketFieldEditor.cpp`, `SmatchetGridUiSupport.cpp`, `tests/CMakeLists.txt`) and #2439 (`SmatchetToast.h`, the undo API). Mitigation: § Implementation handoff orders each slice behind the PRs it collides with, and each slice claims a plan-lock on its write set (`lock-claim.sh`) before coding.
- **Bulk D5 semantics move.** Folding `KeepIssueIds` into the always-on mirror changes the code under bulk P1's selection keep. Mitigation: the existing `bulk_field_edit` bucket-E and `BulkFieldEditPure` cases stay green unchanged (item 17, 20). Bulk P2 / P3 build on `Remap`, so bulk-edit-rows' owner is told in the S1 PR.
- **Paste writes many rows.** `StartBulkFieldEdit` itself never confirms; the confirm lives only in the popup today. So every keyboard bulk write (Ctrl+Enter, Ctrl+D, a range paste) goes through `RequestBulkFieldEdit`, which asks above 10 rows (rows × columns for a multi-column paste) before anything is sent. A bucket-E case pins that an 11-row paste sends nothing until Apply. The per-row skip for non-editable cells and the summary toast come with the bulk job. A paste never targets ADF / long-text fields (they are skipped and counted), and never the key column.
- Save formats, MCP wire format, tracker interfaces, Lua bindings, the Unreal build (`SmatchetCore_DX12` compiles the same TUs; nothing is `SMATCHET_EMBEDDED_IN_UNREAL`-gated): unchanged. New localization keys only, no renames.
- **Non-goals:** redo; formulas or computed cells; fill-series (1, 2, 3…) or drag-fill handles; multi-column bulk in the context-menu popup; Lua / MCP commands for cursor moves (the existing `ticket.set_field*` commands already cover scripted writes).

## Implementation handoff

All slices are owned by `grid-engine` (`Source/Core/src/Ui/AGENTS.md` § Grid). Each slice is one PR, claims a plan-lock on its write set, and lands in this order.

**S1: active cell, keyboard moves, select ≠ edit, selection by identity** (#29 nav half, #30, #32)
- **Write set:** items 1, 3, 4, 5, 6, 7, 8a, 10, 15, 17, 18 (nav cases), 19, 20 (`bulk_field_edit` only), 21, 22. S1 owns the open-on-click gate (items 8a and 10); S2 owns every key that starts or ends an edit.
- **Pre-resolved:** the active cell is (`ActiveIssueId`, `ActiveFieldId`). `ActiveIssueId` keeps its current meaning for the row-level consumers (assistant context, Shift+Space). Arrows reach read-only cells, and only commit-and-move skips them. Keys are claimed with `Shortcut(..., ImGuiInputFlags_RouteFocused | ImGuiInputFlags_Repeat)` or `SetKeyOwner`, never by turning nav off for the window. A cell range is the membership snapshotted by the gesture that made it (`RangeIssueIds` / `RangeFieldIds`), never two corners re-resolved after a sort, so a re-sort can drop cells from a range but never add one. The mobile gesture (`kMobileInlineEditBuild`) is unchanged.
- **Inline inventory:** `SmatchetActiveProjectGridCells.cpp:599-602` (the plain-click `ClearAll`), `:611-615` (the drag extent that never had an anchor), `:649-709` (`drawActiveProjectGridRectSelKeys`), `SmatchetActiveProjectGridTable.cpp:1167-1173` and `:1186-1191` (the two clears), `:818-840` (`DropStaleBulkKeep` / `RestoreBulkKeptRows`), `:1236` (clipper), `TicketFieldEditor.cpp:633` (open gesture), `SpreadsheetState.h:37-64` (`KeepIssueIds`).
- **Order:** first. Starts after #2450 and #2452 merge (shared table / cells / bulk files).

**S2: start and finish an edit from the keys** (#26, #29 edit half)
- **Write set:** items 3 (`StartEditingFieldWithText`), 5, 8, 9, 18 (edit cases), 20 (`duration_inline_edit_commit`).
- **Pre-resolved:** F2 opens with the caret at the end. Enter opens with the value selected (the § 3 single-line contract). A typed character replaces the value (`StartEditingFieldWithText`, baseline = original). Enter / Shift+Enter commit and move down / up, Tab / Shift+Tab commit and move right / left (skip read-only), and Esc reverts and stays. A truncated value (`InlineEditLoadedTruncated`) still refuses the commit and still moves. On a combo cell, F2 / Enter arm the popup, and a typed character opens it with that character in the filter.
- **Inline inventory:** `TicketFieldEditor.cpp:437-548` (`RenderTextInlineEdit`), `:481` (only Esc read), `:744`, `:931`, `:969` (`ArmThenPopupCellGate`), `TicketFieldEditor.cpp:102` (`InputTextCallback_ClearSelectOnEditOpen`, the caret placement on open).
- **Order:** after S1 (needs the active cell).

**S3: undo, the undo toast, and a kept rejected value** (#28 undo, #31, #33, #34)
- **Write set:** items 2, 11, 12, 12a (the per-row overloads only), 12b, 13 (Undo entry), 14, 16, 17 (per-row cases), 18 (undo cases, including a bulk undo that restores A on one row and B on another). S3 is the first user of the per-row bulk input, so it adds it, and S4 reuses it.
- **Pre-resolved:** a single edit's history entry is pushed in `EnqueueGridFieldEdits`, so every single grid write is recorded, including the new-issue row's field edits and the combos. A bulk row is not recorded there: its job pushes one entry. Commit results reach their entry by `PendingFieldEdit::UndoEntryId` and mark it `Landed` / `Failed`, never by an empty-string sentinel, because a cleared field lands with an empty display. An entry with a change still `Pending` is not undone (`StillSaving`). An undo's commit sets `IsUndo` and pushes nothing. The success toast becomes the undo toast, and the error and queued-offline toasts are unchanged (Ui § Toasts say what happened). A bulk job's undo is one entry, which queues one inverse job through the per-row `StartBulkFieldEdit` (each row gets its own `Before` back); it is an undo of a write the user already confirmed, so it does not ask again. It supersedes bulk P2's planned "Revert last bulk edit" menu item: tell that plan's owner (§ Open questions Q2).
- **Inline inventory:** `SmatchetGridFieldEditPipeline.cpp:45` (`PushUnlessBulk`), `:115-118` (the success toast), `:126-138` (the error branch to keep the rejected value), `:275` (`EnqueueGridFieldEdits`), `TicketFieldEditor.cpp:377` (`QueueEdit`).
- **Order:** after #2439 merges (`PushUndoable`). Independent of S2, so S2 and S3 may run in parallel in separate worktrees once S1 is in. Their only shared file is `SmatchetLocalization.cpp`, which takes separate hunks.

**S4: paste, fill-down, Ctrl+Enter apply-to-selection** (#27 rest, #28 paste)
- **Write set:** items 1 (`PlanPaste`), 5 (Ctrl+V / Ctrl+D), 8 (Ctrl+Enter), 12 (`pendingBulkConfirm`), 12a (`RequestBulkFieldEdit` and its confirm), 13 (Paste / Fill down entries), 14, 15 (paste cases), 18 (paste cases, including the 11-row confirm).
- **Pre-resolved:** one value over a range is a fill of its members: `PlanPaste` gets the range's `RowInRange` / `ColInRange` masks, never its corners. A block is placed from the active cell, so a column's cells form one bulk job with per-row values (`BulkRowInput`: each row writes its own pasted cell), and a one-cell column is a single `QueueEdit`. Text is resolved per field kind: choice fields through `BeforeValuesFor` with `AllEntriesResolve` (a miss is skipped and counted), dates and durations through `TrackerParseExplain` (a miss is skipped and counted), plain text as is. ADF / long-text and key columns are skipped. Ctrl+D copies the top row of the range down each column. Ctrl+Enter writes the editor's value to the edited column of every selected row (rows from `Rows` or the range), through one job. Every one of these goes through `RequestBulkFieldEdit`, never straight to `StartBulkFieldEdit`.
- **Order:** after S2 and S3.

## Verification

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**: items 15-17, in the Linux subset (`ninja-test-linux`) and the full `SmatchetTests` on CI. Each case is mutation-checked: break the guarded line on purpose and watch the test fail.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: item 18 on the fixture lane `test-ui-grid-spreadsheet` (item 19), and items 20's re-pinned suites on their existing lanes. One case per audit finding. It drives real key presses through the test engine and asserts the active cell, the editor state, the queued `PendingFieldEdit` values, the toast's undo entry, and the restored value.
- **Bash-driver scenario / screenshot / sanitizer**: `priority-grid-scroll` perf before / after S1. The sanitizer lanes run the new doctests. No golden changes are expected. If the focus rectangle shows in an existing grid golden, re-capture it through the CI bootstrap job, with the reason in the PR.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) per slice.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green for this plan's PR and for every slice that edits docs (items 21-23).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against this plan before S1 starts. The outcome goes in § Verification (actual), with particular attention to the terms "active cell", "range" and "selection" against `GridRectSelection`'s existing vocabulary and bulk-edit-rows D5.
- **Manual residue**: none planned. Clipboard content in bucket E is set through `ImGui::SetClipboardText`, so no OS clipboard is needed.

## Open questions

- **Q1 (user):** Should Enter-down and Tab-right open the neighbour's editor (the audit's § 7.3 wording), or only move the active cell (Excel / Sheets, this plan's default, with type-to-edit as the fast path)? The answer changes S2's `pendingMove` consumer only.
- **Q2 (bulk-edit-rows owner):** With S3, a bulk job's undo is one Ctrl+Z / toast Undo. Should bulk P2 still add the "Revert last bulk edit (N rows)" menu item and `ticket.revert_bulk`, or reuse `GridUndoPure`'s inverse for both? The answer is needed before either S3 or bulk P2 starts, whichever comes first.
- **Q3 (probe, S1):** Does `ImGui::ScrollToRect` with `ImGuiScrollFlags_KeepVisibleEdgeX | EdgeY` keep the active cell clear of the frozen key column and the header row? If not, offset the target by the frozen widths. A bucket-E probe at S1 start answers it.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **#35** (read-only cell affordance and reason) and **#36** (512-byte inline cap, route to the long-text modal): small single-file fixes, not structural. A follow-up PR after S2 (they touch `TicketFieldEditor.cpp`).
- **Redo** (Ctrl+Y / Ctrl+Shift+Z): no audit finding asks for it. `GridUndoPure` can grow a redo list later.
- **Bulk P3's parallel pump** (#34's "coalesce consecutive edits to one issue" half): owned by `bulk-edit-rows` P3. This plan only removes the success toast.
- **Paste from Excel with rich formats, images or HTML**: plain TSV text only.
- **Lua / MCP cursor commands and an Unreal-specific key path**: the Unreal plugin draws the same Core grid, so it inherits the keys. No extra binding is planned.

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
3. *regen the index from the repository root: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
