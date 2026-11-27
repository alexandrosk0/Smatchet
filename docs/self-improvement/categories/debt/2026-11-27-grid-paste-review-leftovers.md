- 2026-11-27 · input-ux-w3-grid-spreadsheet · [debt] · P2 — Grid paste / fill down / Ctrl+Enter (Wave 3, S4) review leftovers: test gaps, block anchor, N single edits for a one-row block

Details:
The code-review of S4 (`SmatchetActiveProjectGridPaste.cpp`, `SmatchetGridBulkRequest.cpp`, `Ui/GridPastePure`) left
these, all recorded in the plan's Deviations (`docs/plans/shipped/input-ux-w3-grid-spreadsheet.md`). Its High (a
function over the line cap) and two Mediums (no cap on the cells of one paste; a number column took any text) were fixed
in the S4 PR.

- **Test gaps (Medium).** The four `Bulk_*` bucket-E cases pin Ctrl+Enter over a range, a one-cell paste, a block paste
  with option lookup and skips, a range fill, fill down, and the 11-cell Cancel / Apply. Nothing covers: the cell menu's
  Paste on a cell outside the range (`RunPaste` re-anchors on the clicked cell); read-only switched on while the confirm
  is open; the undo entry and toast of a single-cell paste; a components column whose rows span two projects; the
  confirm count of a multi-column paste; a full Ctrl+C then Ctrl+V of a date range through `LastGridCopy`; Ctrl+Enter
  with the edited row outside the selection, and from a whole-row selection; any offline case (`GlobalFakeNetwork()`).
  No mutation checks were run on the S4 cases.
- **A block pasted while a range is selected starts at the active cell**, which after Shift+Up or Shift+Left is the
  range's bottom or right corner, so the block lands below or right of the range. The plan says "from the active cell";
  anchoring at the range's top-left member when a range is selected would match a spreadsheet. The cell menu's Paste on
  a cell inside the range also ignores which cell was clicked.
- **A one-row block across N columns is N single edits**: N toasts, N undo entries, N Ctrl+Z presses. One history entry
  for the paste would need an entry that spans fields.
- **Ctrl+Enter's value is dropped with no toast** when the edited cell is no longer shown on the frame the command runs
  (`RunApplyValue`), or the grid emptied. Plain Enter would have committed. In practice the command runs on the frame it
  was set; a fallback single commit would close it.
- **`PendingBulkConfirm` stays active when its pane closes** while the write is parked. Harmless (the next request
  replaces it); clear it when the pane is destroyed.
- **Date detection differs between paste and copy.** `ResolveCellText` tests `field.Type`; `CopyGridRectAsTsv` and the
  undo's `IsRecordable` use `IsTrackerDateOrDateTimeField`. A date-family field with another type is skipped by a paste:
  safe, but not the same rule.
- **`RecordBulkUndoEntry` records every kind except dates**, while the single-edit `IsRecordable` also leaves out an
  Unsupported field that is not a free value. No S4 path reaches the difference today (paste refuses those fields and
  Ctrl+Enter comes only from the text editor); a later caller could record an undo that writes a display name.
- **A number in exponent form does not round-trip.** `ReadsAsNumber` (`GridPastePure.cpp`) takes sign, digits and one
  point. Jira caches a number at or above 1e16 or below 1e-4 as "1e-05" (`JsonValueToCompactString`) and GitHub
  Projects from 1e10 (`%.10g`), so copying such a cell and pasting it back skips it (counted, nothing written). Accept
  an optional `[eE][+-]?digits` suffix after a mantissa digit.
- **Clipboard text that ends in a blank line clears the cell below.** "abc

" is a two-row block for `PlanPaste`:
  "abc" on the active cell and an empty text on the next row. That is right for a grid copy whose last cell is empty
  (`CopyGridRectAsTsv` writes exactly that), and a surprise for text from another app. Drop a final all-empty row
  when the clipboard is not this app's own copy (`IsOwnCopy`).
- **Fill down and Ctrl+Enter are not capped** the way a paste is (`kMaxPasteCells`): a fill over a very large range
  reads and pre-flights every cell on the UI thread before the confirm shows.

Concrete next action:
One PR on `SmatchetActiveProjectGridPaste.cpp` and `tests/ui/grid_spreadsheet_keys.test.cpp`: the bucket-E cases for
the menu Paste outside the range, the date round trip and the multi-column confirm count first, then the range
top-left anchor for a block.

Status: open
Last-reviewed: 2026-11-27
