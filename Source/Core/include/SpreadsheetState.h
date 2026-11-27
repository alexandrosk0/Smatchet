#ifndef SPREADSHEET_STATE_H
#define SPREADSHEET_STATE_H

#include <string>
#include <cstdio>
#include <cstdint>
#include <set>
#include <vector>

// Google-Sheets-style selection model for the ticket grid.
// Two concurrent components:
//   1. A rectangular cell selection (AnchorRow/Col..ExtentRow/Col) used for
//      Shift+drag gestures in the data cells.
//   2. A set of whole-row selections (Rows) populated when the user interacts
//      with the key (ID) column, which supports non-contiguous rows a la
//      Ctrl+click in Sheets.
// Row/col are indices into the current sort order / columns vector; SortSignature
// is snapshotted so the selection self-invalidates when the sort order or ticket
// set changes.
struct GridRectSelection {
    bool Active = false;   // rectangle active
    bool Dragging = false; // mouse held since drag start
    int AnchorRow = -1;
    int AnchorCol = -1;
    int ExtentRow = -1;
    int ExtentCol = -1;
    std::uint64_t SortSignature = 0;

    // Whole-row selections harvested from the key column. Stored as sort-order
    // row indices. Must be invalidated alongside the rectangle on sort change.
    std::set<int> Rows;

    // Sticky anchor row used by Shift+click / Shift+Space extend gestures.
    int PrimaryRow = -1;
    std::string PrimaryIssueId; // the issue PrimaryRow was on when Rows was last mirrored (Remap puts the anchor back)

    // Row-range drag bookkeeping (only meaningful while Dragging && DragRowMode).
    bool DragRowMode = false;
    int DragStartRow = -1;
    std::set<int> DragBaseRows; // rows selected before the current drag started

    // Issue ids to keep selected across a re-sort (bulk edit, docs/plans/bulk-edit-rows.md D5). `Rows` are sort-order
    // indices, which the first landed edit invalidates (the snapshot revision moves, the sort signature changes and
    // the selection is cleared). While KeepIssueIds is set, the grid rebuilds `Rows` from it after such a clear
    // (RestoreKeptRows). `KeepRows` is the `Rows` value the ids were last matched to: when `Rows` differs from it
    // the user made a selection gesture, and KeepIssueIds is dropped (KeepStale). In-memory only.
    std::set<std::string> KeepIssueIds;
    std::set<int> KeepRows;

    bool KeepStale() const { return !KeepIssueIds.empty() && Rows != KeepRows; }

    /// Selects again the rows whose issue id is in KeepIssueIds. `idAt(row)` returns the id (const std::string&)
    /// shown at sort-order row `row`, for rows 0..rowCount-1. Stamps `sortSignature` so the selection counts as
    /// valid for the current order. Returns the number of rows selected; none found drops KeepIssueIds.
    template <class IdAt> int RestoreKeptRows(int rowCount, IdAt idAt, std::uint64_t sortSignature) {
        Rows.clear();
        for (int row = 0; row < rowCount; ++row) {
            if (KeepIssueIds.find(idAt(row)) != KeepIssueIds.end()) {
                Rows.insert(row);
            }
        }
        KeepRows = Rows;
        PrimaryRow = Rows.empty() ? -1 : *Rows.begin();
        SortSignature = sortSignature;
        if (Rows.empty()) {
            KeepIssueIds.clear();
        }
        return static_cast<int>(Rows.size());
    }

    // ---- Selection by identity (input UX Wave 3, grid S1) -----------------------------------------------------
    // A selection is drawn by index and kept by identity. `KeepIssueIds` mirrors `Rows` (always on: SyncRows keeps
    // it equal to Rows while the order is current). A cell range is a MEMBERSHIP fixed by the gesture that made it:
    // the issue ids of its rows (RangeIssueIds) and the field ids of its columns (RangeFieldIds), plus the anchor
    // and extent cells Shift+arrow grows from. After a re-sort the range is the same cells wherever they now sit:
    // a row that moved out of the block stays in, a row that moved between two members does NOT join (corners would
    // add it), and a row that left the view drops out. RowInRange / ColInRange are the per-shown-row / per-column
    // bytes the per-cell test reads (RectContains): they are rebuilt by SetRange and Remap, never per cell.
    std::string AnchorIssueId;
    std::string AnchorFieldId;
    std::string ExtentIssueId;
    std::string ExtentFieldId;
    std::set<std::string> RangeIssueIds;
    std::set<std::string> RangeFieldIds;
    std::vector<char> RowInRange; // one byte per shown row, nonzero = member (empty: the corners alone decide)
    std::vector<char> ColInRange; // one byte per column, nonzero = member
    bool RangeMulti = false;      // more than one cell: only then is the range filled (one cell shows the focus ring)

    /// Mirrors `Rows` into KeepIssueIds when the user (or any gesture) changed it. Call while `Rows` addresses the
    /// CURRENT order (SortSignature == the grid's signature); `idAt(row)` as in RestoreKeptRows. No-op when equal.
    template <class IdAt> void SyncRows(int rowCount, IdAt idAt) {
        if (Rows == KeepRows) {
            return;
        }
        KeepIssueIds.clear();
        for (int row : Rows) {
            if (row >= 0 && row < rowCount) {
                KeepIssueIds.insert(idAt(row));
            }
        }
        KeepRows = Rows;
        PrimaryIssueId = (PrimaryRow >= 0 && PrimaryRow < rowCount && Rows.count(PrimaryRow) != 0) ? idAt(PrimaryRow)
                                                                                                   : std::string();
    }

    /// Makes the range anchor..extent (inclusive, any order) the current range and snapshots its identities.
    /// `idAt(row)` / `fieldAt(col)` return const std::string& for the shown row / drawn column.
    template <class IdAt, class FieldAt>
    void SetRange(int anchorRow, int anchorCol, int extentRow, int extentCol, int rowCount, IdAt idAt, int colCount,
                  FieldAt fieldAt) {
        if (rowCount <= 0 || colCount <= 0 || anchorRow < 0 || anchorCol < 0 || extentRow < 0 || extentCol < 0 ||
            anchorRow >= rowCount || extentRow >= rowCount || anchorCol >= colCount || extentCol >= colCount) {
            Clear();
            return;
        }
        UseBox = false; // the corners are the box until a Remap splits the block
        Active = true;
        AnchorRow = anchorRow;
        AnchorCol = anchorCol;
        ExtentRow = extentRow;
        ExtentCol = extentCol;
        AnchorIssueId = idAt(anchorRow);
        AnchorFieldId = fieldAt(anchorCol);
        ExtentIssueId = idAt(extentRow);
        ExtentFieldId = fieldAt(extentCol);
        RowInRange.assign(static_cast<size_t>(rowCount), 0);
        ColInRange.assign(static_cast<size_t>(colCount), 0);
        RangeIssueIds.clear();
        RangeFieldIds.clear();
        for (int row = MinRow(); row <= MaxRow(); ++row) {
            RowInRange[static_cast<size_t>(row)] = 1;
            RangeIssueIds.insert(idAt(row));
        }
        for (int col = MinCol(); col <= MaxCol(); ++col) {
            ColInRange[static_cast<size_t>(col)] = 1;
            RangeFieldIds.insert(fieldAt(col));
        }
        RangeMulti = (MaxRow() > MinRow()) || (MaxCol() > MinCol());
    }

    /// Re-expresses the selection and the range in a new order (a sort, a sync, a landed edit, a shrunk row set).
    /// Rows come back from KeepIssueIds, the range from RangeIssueIds / RangeFieldIds; whatever is no longer shown
    /// drops out. Stamps `sortSignature`. An in-flight row drag ends (its indices describe the old order).
    template <class IdAt, class FieldAt>
    void Remap(int rowCount, IdAt idAt, int colCount, FieldAt fieldAt, std::uint64_t sortSignature) {
        if (Dragging || DragRowMode) {
            Dragging = false;
            DragRowMode = false;
            DragStartRow = -1;
            DragBaseRows.clear();
        }
        RestoreKeptRows(rowCount, idAt, sortSignature);
        if (!PrimaryIssueId.empty()) {
            // The Shift anchor row follows its issue (RestoreKeptRows put it on the lowest selected row).
            for (int row : Rows) {
                if (idAt(row) == PrimaryIssueId) {
                    PrimaryRow = row;
                    break;
                }
            }
        }
        if (!Active) {
            return;
        }
        RowInRange.assign(static_cast<size_t>(rowCount > 0 ? rowCount : 0), 0);
        ColInRange.assign(static_cast<size_t>(colCount > 0 ? colCount : 0), 0);
        int minRow = -1;
        int maxRow = -1;
        int minCol = -1;
        int maxCol = -1;
        int anchorRow = -1;
        int anchorCol = -1;
        int extentRow = -1;
        int extentCol = -1;
        for (int row = 0; row < rowCount; ++row) {
            const std::string& id = idAt(row);
            if (RangeIssueIds.find(id) == RangeIssueIds.end()) {
                continue;
            }
            RowInRange[static_cast<size_t>(row)] = 1;
            minRow = minRow < 0 ? row : minRow;
            maxRow = row;
            anchorRow = (id == AnchorIssueId) ? row : anchorRow;
            extentRow = (id == ExtentIssueId) ? row : extentRow;
        }
        for (int col = 0; col < colCount; ++col) {
            const std::string& field = fieldAt(col);
            if (RangeFieldIds.find(field) == RangeFieldIds.end()) {
                continue;
            }
            ColInRange[static_cast<size_t>(col)] = 1;
            minCol = minCol < 0 ? col : minCol;
            maxCol = col;
            anchorCol = (field == AnchorFieldId) ? col : anchorCol;
            extentCol = (field == ExtentFieldId) ? col : extentCol;
        }
        if (minRow < 0 || minCol < 0) {
            Clear(); // every row or every column of the range left the view
            return;
        }
        // The corners are the bounding box of the members; the anchor / extent are where their identities now sit,
        // else the box corners (a missing anchor must not leave Shift+arrow growing from nowhere).
        AnchorRow = anchorRow >= 0 ? anchorRow : minRow;
        AnchorCol = anchorCol >= 0 ? anchorCol : minCol;
        ExtentRow = extentRow >= 0 ? extentRow : maxRow;
        ExtentCol = extentCol >= 0 ? extentCol : maxCol;
        // MinRow()/MaxRow() read the corners, which may now sit inside the box: pin the box explicitly.
        BoxMinRow = minRow;
        BoxMaxRow = maxRow;
        BoxMinCol = minCol;
        BoxMaxCol = maxCol;
        UseBox = true;
        int members = 0;
        for (int row = minRow; row <= maxRow && members < 2; ++row) {
            members += RowInRange[static_cast<size_t>(row)] ? 1 : 0;
        }
        RangeMulti = members > 1 || maxCol > minCol;
    }

    // The bounding box of the members after a Remap (the anchor / extent corners can sit inside it once a re-sort
    // split the block). UseBox is false for a range made by SetRange, where the corners are the box.
    int BoxMinRow = -1;
    int BoxMaxRow = -1;
    int BoxMinCol = -1;
    int BoxMaxCol = -1;
    bool UseBox = false;

    int MinRow() const { return UseBox ? BoxMinRow : (AnchorRow < ExtentRow ? AnchorRow : ExtentRow); }
    int MaxRow() const { return UseBox ? BoxMaxRow : (AnchorRow > ExtentRow ? AnchorRow : ExtentRow); }
    int MinCol() const { return UseBox ? BoxMinCol : (AnchorCol < ExtentCol ? AnchorCol : ExtentCol); }
    int MaxCol() const { return UseBox ? BoxMaxCol : (AnchorCol > ExtentCol ? AnchorCol : ExtentCol); }

    /// True when (r, c) is in the range: inside its box and, when the range has a membership, a member row and a
    /// member column. Two byte reads, no allocation.
    bool RectContains(int r, int c) const {
        if (!Active || r < MinRow() || r > MaxRow() || c < MinCol() || c > MaxCol()) {
            return false;
        }
        if (!RowInRange.empty() &&
            (static_cast<size_t>(r) >= RowInRange.size() || !RowInRange[static_cast<size_t>(r)])) {
            return false;
        }
        if (!ColInRange.empty() &&
            (static_cast<size_t>(c) >= ColInRange.size() || !ColInRange[static_cast<size_t>(c)])) {
            return false;
        }
        return true;
    }
    bool RowSelected(int r) const { return Rows.find(r) != Rows.end(); }
    bool Contains(int r, int c) const { return RectContains(r, c) || RowSelected(r); }

    bool HasAnySelection() const { return Active || !Rows.empty(); }

    void Clear() {
        Active = false;
        Dragging = false;
        AnchorRow = AnchorCol = ExtentRow = ExtentCol = -1;
        AnchorIssueId.clear();
        AnchorFieldId.clear();
        ExtentIssueId.clear();
        ExtentFieldId.clear();
        RangeIssueIds.clear();
        RangeFieldIds.clear();
        RowInRange.clear();
        ColInRange.clear();
        RangeMulti = false;
        BoxMinRow = BoxMaxRow = BoxMinCol = BoxMaxCol = -1;
        UseBox = false;
    }
    void ClearAll() {
        Clear();
        Rows.clear();
        PrimaryRow = -1;
        PrimaryIssueId.clear();
        DragRowMode = false;
        DragStartRow = -1;
        DragBaseRows.clear();
    }
};

/// How a key opens the editor of the active cell (the request SpreadsheetState::KeyEdit carries to that cell's draw).
enum class GridEditStart {
    None,
    CaretAtEnd, // F2: the caret at the end of the value
    SelectAll,  // Enter: the value selected, so typing replaces it
    Typed       // a printable character: the buffer starts as that text (type-to-edit replaces the value)
};

/// Where the active cell goes after an inline editor ends on a key: Enter / Shift+Enter commit and move down / up,
/// Tab / Shift+Tab commit and move right / left. Reported by the editor, consumed by the grid's cursor keys.
enum class GridEditExit { None, Down, Up, Right, Left };

/// A keyboard bulk write asked for outside the grid's key handler: Ctrl+Enter in an inline editor (ApplyValue), or the
/// cell menu's Paste and Fill down. The cursor keys run it after the row loop, then clear it.
enum class GridBulkCommand { None, ApplyValue, Paste, FillDown };

struct SpreadsheetState {
    std::string ActiveIssueId;
    // The active cell is (ActiveIssueId, ActiveFieldId): kept by identity so it survives a re-sort, a sync and a
    // landed edit. ActiveRow / ActiveCol are the derived indices the per-cell code compares (two ints, no
    // allocation); they are rebuilt when the order, the issue or the field changes (ResolveActive) and set directly
    // by the gestures that move the cell. ActiveFieldId "" = no column yet (the first column).
    std::string ActiveFieldId;
    int ActiveRow = -1;
    int ActiveCol = -1;
    /// The (sort signature, issue, field) the indices above were last resolved for.
    std::uint64_t ActiveResolvedSig = 0;
    std::string ActiveResolvedIssueId;
    std::string ActiveResolvedFieldId;
    /// The shown-rows revision (GridPane::filteredRowsRevision) the grid last saw, and the one the active cell's
    /// indices were resolved against: a rebuilt row projection (a search, a filter) moves rows without touching the
    /// signature.
    std::uint64_t ShownRowsRevision = 0;
    std::uint64_t ActiveResolvedRevision = 0;
    /// A keyboard move asks the grid to scroll this cell into view (the draw of that cell consumes it). -1 = none.
    int ScrollToRow = -1;
    int ScrollToCol = -1;
    /// Rows a PageUp / PageDown moves: the body height in rows, measured by the table each frame.
    int PageRows = 10;
    /// A key asked to open the editor of one cell (issue, field); the cell's own draw consumes it on the next frame, so
    /// every editor kind opens through the code that already owns it. Unconsumed requests (the cell is scrolled out
    /// of view, read-only) are dropped by the key handler after that frame.
    GridEditStart KeyEditKind = GridEditStart::None;
    std::string KeyEditIssueId;
    std::string KeyEditFieldId;
    std::string KeyEditSeed;
    /// Frames the request has waited for its cell (the cell may first have to be scrolled into view).
    int KeyEditFrames = 0;
    /// Set by an inline editor that ended on Enter / Tab; the cursor keys move the active cell on it, then clear it.
    GridEditExit PendingEditExit = GridEditExit::None;
    /// The pending bulk command. ApplyValue carries the edited cell (issue, field) and the value to write.
    GridBulkCommand BulkCommand = GridBulkCommand::None;
    std::string BulkIssueId;
    std::string BulkFieldId;
    std::string BulkValue;
    /// Whether an editor, a dropdown or a popup was open at the end of the last frame: a key that closed one this
    /// frame (Enter, Esc) must not also be read as "start an edit".
    bool EditBusyLastFrame = false;
    std::string EditingId;
    std::string EditingFieldId;
    int EditingColumn = -1;
    bool EditJustStarted = false;
    /** Set when grid cell text edit opens; ImGui may defer `EventActivated` until after `EditJustStarted` clears. */
    bool PendingGridInputTextDeselect = false;
    /** Enter opened the editor: select the whole value on its first active frame, so typing replaces it. */
    bool PendingGridInputTextSelectAll = false;
    char EditBuffer[512] = "";
    /** The buffer's content at edit-start — the dirty-check baseline for the inline editor. The
     *  value AS LOADED into EditBuffer, captured once so the "did the user change it?" verdict is
     *  stable: a background grid refresh / sync can mutate the field's live value mid-edit, and
     *  comparing the buffer against that live value would read an untouched edit as dirty → spurious
     *  PUT. Captured in CopyToEditBuffer, cleared in ClearEditing. */
    std::string EditInitialValue;
    /** A cell whose last save the tracker rejected hands its rejected value and message to the editor that opens on it
     *  (input UX Wave 3, S3). The cell draw points these at the feedback entry for the span of its RenderFieldCell call
     *  only (null otherwise); the text editor copies the value into EditBuffer on its first frame and keeps the message
     *  to draw under the field while it is open. */
    const std::string* CellRejectedValue = nullptr;
    const std::string* CellRejectedMessage = nullptr;
    std::string EditRejectedMessage;
    /** The cell whose editor was reseeded from a rejected value. Kept past the edit's end so the grid can tell an Esc
     *  (the rejected value is dropped) from a commit (the next save replaces the feedback anyway). */
    std::string EditRejectedIssueId;
    std::string EditRejectedFieldId;
    GridRectSelection RectSel;

    void SetActiveIssue(const std::string& id) { ActiveIssueId = id; }

    /// Makes (issueId, fieldId) the active cell at shown row `row` / drawn column `col` for the order `sig`, and
    /// marks the indices resolved so the next frame does not search for them again.
    void SetActiveCell(const std::string& issueId, const std::string& fieldId, int row, int col, std::uint64_t sig) {
        ActiveIssueId = issueId;
        ActiveFieldId = fieldId;
        ActiveRow = row;
        ActiveCol = col;
        ActiveResolvedSig = sig;
        ActiveResolvedRevision = ShownRowsRevision;
        ActiveResolvedIssueId = issueId;
        ActiveResolvedFieldId = fieldId;
    }
    /// No active cell (a click on empty space, Ctrl+click off the active row). Leaves the selection alone.
    void ClearActiveCell() {
        ActiveIssueId.clear();
        ActiveFieldId.clear();
        ActiveRow = ActiveCol = -1;
        ActiveResolvedSig = 0;
        ActiveResolvedRevision = 0;
        ActiveResolvedIssueId.clear();
        ActiveResolvedFieldId.clear();
        ScrollToRow = ScrollToCol = -1;
    }
    /// Rebuilds ActiveRow / ActiveCol from the identity when the order, the issue or the field changed since they
    /// were last resolved. Once a frame, before the row loop: three string compares when nothing changed. A field
    /// that is not shown, or an issue that is not shown, leaves that index -1 (no active cell is drawn).
    template <class IdAt, class FieldAt>
    void ResolveActive(int rowCount, IdAt idAt, int colCount, FieldAt fieldAt, std::uint64_t sig) {
        if (ActiveResolvedSig == sig && ActiveResolvedRevision == ShownRowsRevision &&
            ActiveResolvedIssueId == ActiveIssueId && ActiveResolvedFieldId == ActiveFieldId) {
            return;
        }
        ActiveResolvedSig = sig;
        ActiveResolvedRevision = ShownRowsRevision;
        ActiveResolvedIssueId = ActiveIssueId;
        ActiveResolvedFieldId = ActiveFieldId;
        ActiveRow = -1;
        ActiveCol = -1;
        if (ActiveIssueId.empty()) {
            return;
        }
        for (int row = 0; row < rowCount; ++row) {
            if (idAt(row) == ActiveIssueId) {
                ActiveRow = row;
                break;
            }
        }
        if (ActiveRow < 0 || colCount <= 0) {
            return;
        }
        if (ActiveFieldId.empty()) {
            ActiveCol = 0;
            return;
        }
        for (int col = 0; col < colCount; ++col) {
            if (fieldAt(col) == ActiveFieldId) {
                ActiveCol = col;
                break;
            }
        }
    }
    void ClearEditing() {
        EditingId = "";
        EditingFieldId = "";
        EditingColumn = -1;
        EditJustStarted = false;
        PendingGridInputTextDeselect = false;
        PendingGridInputTextSelectAll = false;
        EditInitialValue.clear();
        EditRejectedMessage.clear();
    }

    void StartEditing(const std::string& id, int col, const std::string& val) {
        EditingId = id;
        EditingFieldId.clear();
        EditingColumn = col;
        EditJustStarted = true;
        PendingGridInputTextDeselect = true;
        CopyToEditBuffer(val);
    }

    void StartEditingField(const std::string& id, const std::string& fieldId, const std::string& val) {
        EditingId = id;
        EditingFieldId = fieldId;
        EditingColumn = -1;
        EditJustStarted = true;
        PendingGridInputTextDeselect = true;
        CopyToEditBuffer(val);
    }

    /// Type-to-edit: the buffer holds `seed` (the typed text replaces the value) while the dirty baseline stays the
    /// original value as loaded (truncated like the buffer), so an edit that ends equal to it never PUTs.
    void StartEditingFieldWithText(const std::string& id, const std::string& fieldId, const std::string& originalValue,
                                   const std::string& seed) {
        StartEditingField(id, fieldId, originalValue);
        std::snprintf(EditBuffer, sizeof(EditBuffer), "%s", seed.c_str());
        EditBuffer[sizeof(EditBuffer) - 1] = '\0';
    }

    void ClearBulkCommand() {
        BulkCommand = GridBulkCommand::None;
        BulkIssueId.clear();
        BulkFieldId.clear();
        BulkValue.clear();
    }

    void ClearKeyEdit() {
        KeyEditKind = GridEditStart::None;
        KeyEditIssueId.clear();
        KeyEditFieldId.clear();
        KeyEditSeed.clear();
        KeyEditFrames = 0;
    }

    bool IsEditingField(const std::string& id, const std::string& fieldId) const {
        return EditingId == id && EditingFieldId == fieldId;
    }

    /** Double-click arm-then-popup: in double-click-to-edit mode, a double-clicked combo cell
     *  writes its `ticket.id + "::" + field.Id` here on frame N. On frame N+1 the matching
     *  per-type editor sees the key, opens the popup once, and clears the field on combo close. */
    std::string EditArmedKey;
    /** True on the single frame after EditArmedKey is set; the per-type editor force-opens the
     *  combo popup once and resets this flag. Used to distinguish "first frame after arm"
     *  (force-open the popup) from "later frames where the popup is closed" (user dismissed →
     *  release arm). */
    bool EditArmedJustOpened = false;

  private:
    void CopyToEditBuffer(const std::string& val) {
        std::snprintf(EditBuffer, sizeof(EditBuffer), "%s", val.c_str());
        EditBuffer[sizeof(EditBuffer) - 1] = '\0';
        // Baseline from the (possibly truncated) buffer content, so the dirty check stays exact even
        // when val exceeds the buffer — the live buffer can only ever equal the truncated start.
        EditInitialValue = EditBuffer;
    }
};

#endif
