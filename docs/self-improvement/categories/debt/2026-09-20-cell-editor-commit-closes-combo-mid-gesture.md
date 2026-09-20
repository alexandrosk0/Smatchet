- 2026-09-20 · code-review · [debt] · P2 — a grid cell editor's first queued edit closes its own popup, so no multi-toggle gesture can live there

  Details: the multi-select / components list (`TicketFieldEditor.cpp` `RenderMultiSelectComboBody`)
  and the label-suggestion list (`TrackerLabelsEditor.cpp` `DrawLabelsComboBody`) queue a set-REPLACE
  edit on every checkbox toggle. `EnqueueGridFieldEdits` keeps only the latest queued edit per cell
  (`SmatchetGridFieldEditPipeline.cpp`), `PumpGridFieldEdits` then dispatches it and sets the cell to
  `CellWriteState::Saving`, and `SmatchetActiveProjectGridCells.cpp` renders the read-only Saving cell
  INSTEAD of `RenderFieldCell` — so the open combo disappears the moment the first toggle commits.
  Each editor also rebuilds its selection from `currentValue` every frame, so nothing accumulates
  across frames either. Ticking three labels therefore costs three separate open-click-commit cycles
  and three round trips, and any multi-row gesture (drag-to-paint per
  docs/plans/drag-to-paint-checkbox-lists.md) dies after the frame it starts in. Found while wiring
  `SmatchetDragCheckbox` into those two lists (PR #2230) — the call sites were reverted for this
  reason; the Views > Fields list is unaffected because its selection set lives on the session.

  Concrete next action: hold the editor's selection in the editor's own state for as long as the popup
  is open (seeded from `currentValue` on open, keyed by `editorKey`, which
  `state.MultiSelectActiveKey` / `activeEditorKey` already track) and queue ONE edit when the popup
  closes, rather than one per toggle. That keeps the latest-per-cell queue semantics, drops N-1 round
  trips per multi-select edit, and makes a drag-to-paint run viable in both editors.
  Status: open
  Last-reviewed: 2026-09-20
