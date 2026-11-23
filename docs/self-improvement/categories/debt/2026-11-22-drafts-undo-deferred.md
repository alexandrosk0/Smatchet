- 2026-11-22 · input-ux-w2-drafts-undo · [debt] · P2 — Drafts and Undo leftovers: grid edit undo, view-delete Undo, conflict-discard undo and three draft surfaces

Details:
`docs/plans/input-ux-w2-drafts-undo.md` (Wave 2 piece 4.3, PR #2439) added the draft store and the toast Undo
(`SmatchetToast::PushUndoable`, Ctrl+Z). These were left out because another plan-lock held their files; those
locks have since been released:

- **Grid cell edit undo (#28 single cell, #33 blur commit) and keeping a rejected value (#31).** Files:
  `SmatchetGridFieldEditPipeline.cpp`, `TicketFieldEditor.cpp` and the grid cell TUs. The toast API is what it
  plugs into. Wave 3's `docs/plans/input-ux-w3-grid-spreadsheet.md` also lists #28, #31 and #33 as closed by its
  design (with paste and ranges); whichever lands first, the other drops them.
- **View-delete Undo (#90).** One function plus a call in `SmatchetViewsDashboardUi.cpp`: re-create the captured
  definition through the create latch, refuse if the tracker changed, bring it back last and active, and sync the
  grid to the restored view (Bugbot's note on the dropped version). The dropped version is in #2439's branch
  history as `PushViewDeletedUndoToast`.
- **Conflict "Discard my edit" undo (#116)** and undo for discarding queued offline rows. A row held for undo could
  replay during the window, so this needs a queue-service entry point in `Sync/` (archive to dead letters, restore
  by original id).
- **New-issue row and Quick Create drafts (#51's other half):** `SmatchetNewIssueDraftUi.cpp`,
  `SmatchetQuickCreateIssueUi.cpp`.
- **Worklog Time remaining** is not kept in the Log work draft (only the spent time and comment are).
- **`DraftStore` worker tests** live in bucket E (`DraftStore_PersistsWithoutStopAndReloads`) because
  `DraftStore.cpp` is not in `SmatchetTests`' source list; add it and move the worker cases back into
  `tests/Core/IssueDraft.test.cpp`.
- **Dictation into the long-text editor** changes the buffer without a text-changed event, so it is not stored
  until the next typed edit.
- **fr-FR rows** for the new draft and undo strings (they render the English fallback), and the unused old
  `offline.conflict` "kept until Smatchet closes" row.
- Related open Issues: #2458 (dictation reload drain) and #2459.

Concrete next action:
Ship view-delete Undo first (smallest), then grid cell edit undo, then the `Sync/` archive/restore entry point for
#116, then the new-issue and Time-remaining drafts, each in its own PR with a bucket-E case beside the existing
`OfflineFirst/*` and toast Undo cases. Delete each bullet as it lands.

Status: open
Last-reviewed: 2026-11-22
