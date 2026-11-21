- 2026-11-16 · input-ux-w2-gesture-keys · [debt] · P3 — column resize on the read-only tables has no keyboard equivalent

Details:
The audit, performance, offline-queue, annotate, bulk-import and Markdown-preview tables set
`ImGuiTableFlags_Resizable`, so their column dividers are mouse-drag only. `scripts/dev/test-drag-keyboard-equivalent.sh`
does not look for that flag (it covers `BeginDragDropSource`, `IsMouseDragging`, `GetMouseDragDelta` and
`ImGuiTableFlags_Reorderable`), because a keyboard resize here needs a table-generic command and a focused-column
notion that the grid-column work (`grid.column.*`, deferred in `docs/plans/input-ux-w2-gesture-keys.md`) will define first.

Concrete next action:
- After the grid column commands land, add one `ui.table.column_grow` / `ui.table.column_shrink` pair on the same
  focus-scoped latch (`Ui/GestureKeys.h`), consumed through a small helper that wraps `ImGui::TableSetColumnWidth` for
  the focused table's current column, and add `ImGuiTableFlags_Resizable` to the script's pattern with each table marked.

Status: open
Last-reviewed: 2026-11-16
