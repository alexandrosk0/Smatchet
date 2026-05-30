#ifndef SPREADSHEET_STATE_H
#define SPREADSHEET_STATE_H

#include <string>
#include <cstdio>
#include <cstdint>
#include <set>

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

    // Row-range drag bookkeeping (only meaningful while Dragging && DragRowMode).
    bool DragRowMode = false;
    int DragStartRow = -1;
    std::set<int> DragBaseRows; // rows selected before the current drag started

    int MinRow() const { return AnchorRow < ExtentRow ? AnchorRow : ExtentRow; }
    int MaxRow() const { return AnchorRow > ExtentRow ? AnchorRow : ExtentRow; }
    int MinCol() const { return AnchorCol < ExtentCol ? AnchorCol : ExtentCol; }
    int MaxCol() const { return AnchorCol > ExtentCol ? AnchorCol : ExtentCol; }

    bool RectContains(int r, int c) const {
        return Active && r >= MinRow() && r <= MaxRow() && c >= MinCol() && c <= MaxCol();
    }
    bool RowSelected(int r) const { return Rows.find(r) != Rows.end(); }
    bool Contains(int r, int c) const { return RectContains(r, c) || RowSelected(r); }

    bool HasAnySelection() const { return Active || !Rows.empty(); }

    void Clear() {
        Active = false;
        Dragging = false;
        AnchorRow = AnchorCol = ExtentRow = ExtentCol = -1;
    }
    void ClearAll() {
        Clear();
        Rows.clear();
        PrimaryRow = -1;
        DragRowMode = false;
        DragStartRow = -1;
        DragBaseRows.clear();
    }
};

struct SpreadsheetState {
    std::string ActiveIssueId;
    std::string EditingId;
    std::string EditingFieldId;
    int EditingColumn = -1;
    bool EditJustStarted = false;
    /** Set when grid cell text edit opens; ImGui may defer `EventActivated` until after `EditJustStarted` clears. */
    bool PendingGridInputTextDeselect = false;
    char EditBuffer[512] = "";
    GridRectSelection RectSel;

    void SetActiveIssue(const std::string& id) { ActiveIssueId = id; }
    void ClearEditing() {
        EditingId = "";
        EditingFieldId = "";
        EditingColumn = -1;
        EditJustStarted = false;
        PendingGridInputTextDeselect = false;
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

    bool IsEditingField(const std::string& id, const std::string& fieldId) const {
        return EditingId == id && EditingFieldId == fieldId;
    }

    /** Per-instance state for the multi-select combo search box.
     *  Previously block-scope statics in RenderMultiSelectEditor — shared across all combos,
     *  which aliased the buffer when two different editors were open in the same frame (§3.4). */
    std::string MultiSelectActiveKey;
    char MultiSelectSearchBuf[128] = {};

    /** Per-instance state for the single-select combo quick-filter box.
     *  Same lifetime pattern as `MultiSelect*`: keyed on `ticket.id + "::" + field.Id` so the
     *  buffer auto-clears when the user opens a different cell's dropdown. */
    std::string SingleSelectActiveKey;
    char SingleSelectSearchBuf[128] = {};

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
    }
};

#endif
