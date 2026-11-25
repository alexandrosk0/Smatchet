- 2026-11-22 · input-ux-w2-drafts-undo · [debt] · P2 — Drafts and Undo leftovers: grid edit undo, queued-row discard undo and three draft surfaces

Details:
`docs/plans/input-ux-w2-drafts-undo.md` (Wave 2 piece 4.3, PR #2439) added the draft store and the toast Undo
(`SmatchetToast::PushUndoable`, Ctrl+Z). These were left out because another plan-lock held their files; those
locks have since been released:

- **Grid cell edit undo (#28 single cell, #33 blur commit) and keeping a rejected value (#31).** Files:
  `SmatchetGridFieldEditPipeline.cpp`, `TicketFieldEditor.cpp` and the grid cell TUs. The toast API is what it
  plugs into. Wave 3's `docs/plans/input-ux-w3-grid-spreadsheet.md` also lists #28, #31 and #33 as closed by its
  design (with paste and ranges); whichever lands first, the other drops them. Now owned by that plan's slice S3.
- **Keyboard guide line for view-delete Undo.** `docs/guides/keyboard-shortcuts.md` ("A confirmed action can be
  taken back...") gains "deleting a view" once the `input-ux-w3-grid-s1` plan-lock on that file is released.
- **Undo for discarding queued offline rows** (the queue panel's row Delete and bulk delete). A row held for undo
  could replay during the window, so this needs a queue-service entry point in `Sync/` (archive to dead letters,
  restore by original id). The conflict resolver's "Discard my edit" (#116) shipped without one, because conflict
  rows never replay (`docs/plans/shipped/input-ux-w2-conflict-discard-undo.md`).
- **Worklog Time remaining restore has no bucket-E case.** `docs/plans/shipped/input-ux-w2-create-drafts.md`
  keeps it (unit-tested through `WorklogRemainingDraftText`). The case belongs in
  `tests/ui/duration_inline_edit_commit.test.cpp`, held by the `input-ux-w3-grid-s2` plan-lock: type a Time
  remaining, close Log work, reopen, and check it comes back with the restored line and Clear puts the estimate back.
- **The queue's "Open as draft" drops a finished row create without settling the kept draft.**
  `OpenDeadLetterRowAsNewIssueDraft` and `OpenPendingCreateRowAsNewIssueDraft` (`SmatchetOfflineQueueUi.cpp`) take
  a ready `newIssueCreateFuture` with `.get()` and never call the draft row's settle step, so a create that landed
  in that one frame leaves its kept entry, which the next "+ New Issue" restores. Expose the settle step from
  `SmatchetNewIssueDraftUi.cpp` and call it there (the conflict-discard lock held that file while the create
  drafts shipped).
- **`DraftStore` worker tests** live in bucket E (`DraftStore_PersistsWithoutStopAndReloads`) because
  `DraftStore.cpp` is not in `SmatchetTests`' source list; add it and move the worker cases back into
  `tests/Core/IssueDraft.test.cpp`.
- **Dictation into the long-text editor** changes the buffer without a text-changed event, so it is not stored
  until the next typed edit.
- **fr-FR rows** for the new draft and undo strings (they render the English fallback, including the create
  drafts' `newissue.draft_restored*`, `quickcreate.draft_*` and `draft.kept_on_view_change`), and the unused old
  `offline.conflict` "kept until Smatchet closes" row.
- Related open Issues: #2458 (dictation reload drain) and #2459.

Concrete next action:
View-delete Undo, the conflict "Discard my edit" Undo (#116) and the create drafts shipped
(`docs/plans/shipped/input-ux-w2-view-delete-undo.md`, `docs/plans/shipped/input-ux-w2-conflict-discard-undo.md`,
`docs/plans/shipped/input-ux-w2-create-drafts.md`). Next: the queue's "Open as draft" settle step, the `Sync/`
archive/restore entry point for the queue panel's row Delete, and the Time remaining bucket-E case once the S2 lock
is released, each in its own PR with a bucket-E case beside the existing `OfflineFirst/*` and toast Undo cases.
Delete each bullet as it lands.

Status: open
Last-reviewed: 2026-11-24
