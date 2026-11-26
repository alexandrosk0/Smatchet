- 2026-11-25 · input-ux-w3-grid-spreadsheet · [debt] · P2 — Grid key-edit (Wave 3, S2) review mediums: Enter/Space claimed with no editable cell, typed characters lost, dropdown type-to-edit untested

Details:
The code-review of S2 (PR #2483, `SmatchetActiveProjectGridKeys.cpp`) found three Medium items that merged unchanged:

- **Enter / Space / F2 / KeypadEnter are owner-claimed every eligible frame** (`kEditKeys`, `SmatchetActiveProjectGridKeys.cpp`
  near the claim at line ~305), even with no active cell or a read-only one. ImGui nav activation needs no key owner, so Enter or
  Space on any other focusable item in the grid window (toolbar, ID selectable) does nothing. Claim the keys only when the active
  cell passes `GridCellEditable`.
- **Fast typing can lose characters.** While a `KeyEdit` request waits (an off-screen cell scrolls in, up to 3 frames) a second
  typed character overwrites `KeyEditSeed`, so the first is lost; a character landing in the frame the editor opens, before the
  InputText is active, is dropped. The test only covers `qr` typed in one frame. Append to the pending seed when the request
  targets the same cell.
- **Dropdown type-to-edit test gap** (`tests/ui/grid_spreadsheet_keys.test.cpp` near line 747): the case asserts only that the popup
  opens; nothing pins that the re-queued character (`AddInputCharactersUTF8`) reaches the dropdown filter, which
  `docs/guides/keyboard-shortcuts.md` promises.

Five Low nits came with it: `GridCellEditable` runs every eligible frame before any key is known pressed; `StartEditingField`
does not reset `PendingGridInputTextSelectAll`; `InlineEditTabPressed` ignores Ctrl (Ctrl+Tab in a cell editor commits and
moves, and blocks ImGui window switching); `TypedSeed` reads `io.KeyCtrl` / `io.KeyAlt` instead of `ImGuiEffectiveKeyCtrl`
(Unreal host); the editable RenderPlan set is listed twice (`GridCellEditable` and `StartEditFromKey`).

Concrete next action:
One PR on `SmatchetActiveProjectGridKeys.cpp` / `TicketFieldEditor.cpp` / `SpreadsheetState.h`: the editable-gated owner claim,
the seed append, a bucket-E case that types into a dropdown cell and asserts the filter text, then the Low nits.

Status: open
Last-reviewed: 2026-11-25
