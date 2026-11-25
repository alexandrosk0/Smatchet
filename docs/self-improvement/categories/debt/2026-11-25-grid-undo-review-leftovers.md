- 2026-11-25 · input-ux-w3-grid-spreadsheet · [debt] · P2 — Grid undo (Wave 3, S3) review leftovers: cross-pane Ctrl+Z, backend switch, local-apply failures, offline test

Details:
The code-review of S3 (`SmatchetGridFieldEditPipeline.cpp`, `GridUndoPure.h`) left these Mediums and Lows, all recorded in
the plan's Deviations:

- **Ctrl+Z defers to any live undoable toast**, and the toast manager undoes the newest toast globally, so within 6 s
  Ctrl+Z in pane A can undo pane B's edit or an AI-assistant / offline-queue action. Scope the toast Undo to the pane, or
  have the grid's Ctrl+Z press only its own entry's toast.
- **A pane's backend switch does not clear `gridUndo`.** A single undo writes to the target latched at edit time (the old
  tracker); a bulk undo re-latches the pane's current binding in `StartJobCore`. Clear or scope the pane's entries on a
  switch, and pass `GridUndoContext::Target` to the bulk path.
- **A saved edit whose local apply failed is not undoable** (`SettleUndoChange(false)`), against "landed = saved online or
  queued offline" in `GridUndoPure.h`. Mark it landed, or list it as a deliberate exclusion.
- **Test gaps:** no bucket-E case for the undo conflict drop (`FilterUndoConflicts`: a cell changed after it was saved is left alone and a warning toast shows); no offline undo case (bucket A with `GlobalFakeNetwork()`, or the bucket-E offline lane) and nothing
  covering the real `RejectedValue` assignment in `ApplyCommitResultOnUiThread`; the bucket-E case seeds the feedback by
  hand.
- Lows: no `LOG_DEBUG` on the undo branches; a bulk undo job's "Saving N rows" / nothing-to-send toasts still say "Bulk edit: X";
  a reseeded editor that loses focus with no keystroke sends the rejected value again (`TicketFieldEditor.cpp`, the
  deactivated path); two quick edits of one cell capture the same pre-edit value, so the first Ctrl+Z skips back two steps.

Concrete next action:
One PR on `SmatchetGridFieldEditPipeline.cpp` / `GridUndoPure.h` / the Ctrl+Z handler: the pane-scoped toast Undo and the
backend-switch clear first (both are wrong-target risks), then the offline bucket-A case.

Status: open
Last-reviewed: 2026-11-25
