# Plan — Input UX Wave 2 leftover: Undo on the conflict resolver's "Discard my edit"

> **Slug**: `input-ux-w2-conflict-discard-undo` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Audit finding #116 (`docs/audits/INPUT_UX_AUDIT.md`, § 7.10): in the offline conflict resolver, "Discard my edit" hard-deletes the queued field edit in one click (`SmatchetOfflineQueueUi.cpp`, `DrawConflictPaneUnverified` and `DrawConflictPaneUnknown`), with no confirm and no undo. The button sits next to "Force Mine". Wave 2's drafts-and-undo piece (`docs/plans/shipped/input-ux-w2-drafts-undo.md`, PR #2439) added the toast Undo (`SmatchetToastManager::PushUndoable`, Ctrl+Z) but left #116 out. Its reasoning was that a row held for undo could replay during the window, so a safe undo would need an archive/restore entry point in `Sync/`.

That reasoning does not hold for this button. The resolver only opens on a pending field edit marked `has_merge_conflict` (`IsResolvableConflictRow`), and the replay skips such rows: `OfflineQueueService::TickOfflineFieldEdits` starts no pass for them (`HasReplayableRow` with `!row.HasMergeConflict`) and `continue`s past them in the loop. The only path that clears the mark is the resolver's own resolve (`ResolveFieldEditConflict`), which a held row can no longer reach, because the held row is hidden from the panel. So the row can stay where it is during the Undo window, and the discard can wait for the toast to settle.

Intended outcome: "Discard my edit" closes the resolver and shows a "Edit discarded" toast with Undo and Ctrl+Z. Undo puts the conflict row back in the panel, unchanged. Otherwise the row is deleted when the toast settles, exactly as today.

## Approach

- **Hold, don't delete.** The discard adds the row id to a file-static held set (`g_conflictDiscardHeld`, UI thread only) and pushes `PushUndoable("Edit discarded", …, 8000, undo, commit)`.
  - `undo` takes the id out of the set, so the row shows again.
  - `commit` takes it out and calls `AppController::DeletePendingFieldEdits({id})`, which writes the audit entry as today (ADR-0016: dead letters stay failures-only).
  - The toast settles as a commit on its timeout, the live cap, `DismissAllLive` and at shutdown (`CommitPendingUndos`, which runs while the services exist).
- **Hidden while held.** `BuildUnifiedOfflineRows` output is filtered by the held set, so the row cannot be opened, resolved, selected, copied or deleted from the panel during the window. A selection holding it is pruned by the existing `PruneOfflineSelectionToLiveRows`.
- **Cache swap guard.** Row ids are global in the one local cache DB (`AUTOINCREMENT`). The commit captures a `std::weak_ptr` to `app.Cache` at the discard. If the cache was recreated in between ("Recreate local cache database"), the commit only logs and deletes nothing: the old row went with the old DB, and an id in the new DB is not the one discarded.
- **Commit failure.** If `DeletePendingFieldEdits` reports `Failed > 0`, the commit logs a warning and arms the panel status "The edit could not be discarded; it is still queued." (it stays a conflict row, so nothing replays it).
- **Both buttons.** The unverified and unknown panes share one helper, `DiscardConflictEditWithUndo`, so they behave the same. The panel status line "Edit discarded." is dropped, because the toast says it.

Not done here: the audit's alternative (move Discard away from the primary buttons) is not needed once the click can be undone. Undo for the queue panel's own row Delete (non-conflict rows, which do replay) still needs the `Sync/` entry point and stays deferred.

## Interface contracts

N/A: no tracker, command, MCP, Lua or storage interface changes. `DeletePendingFieldEdits` is called as today, only later. No `UiDrawSession` field is added (the held set is file-static in the offline-queue TU, like `g_selectedOfflineRowKeys`).

## Files to modify

1. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp`: `g_conflictDiscardHeld`, `DiscardConflictEditWithUndo`, the filter after `BuildUnifiedOfflineRows`, both "Discard my edit" call sites, and the test seam definitions.
2. `Source/Core/include/Ui/SmatchetOfflineQueueActionsUi.h`: declarations of the test seams `SmatchetOfflineConflictDiscardHeldCountForTest()` and `SmatchetOfflineConflictDiscardHeldClearForTest()`, next to the existing `SmatchetOfflineConflictDraft*ForTest` seams.
3. `tests/ui/offline_conflict_modal_panes.test.cpp`: one bucket-E case in its existing registration (no registry or CMake edit).
4. `docs/plans/active/input-ux-w2-conflict-discard-undo.md`: this plan (moved to `shipped/` on ship), and `docs/plans/INDEX.md`.
5. `docs/self-improvement/categories/debt/2026-11-22-drafts-undo-deferred.md`: narrow the #116 bullet to the queue panel's own row Delete.

## Existing utilities reused

- `SmatchetToastManager::PushUndoable` / `UndoNewest` / `LiveUndoableCount` / `HasLive` (`Ui/SmatchetToast.h`).
- `AppController::DeletePendingFieldEdits` (audit entry included), `AppController::Cache` (atomic shared_ptr).
- `FinishConflictModal`, `ArmOfflineQueuePanelStatus`, `PruneOfflineSelectionToLiveRows`.
- The test file's `EnterConflictHost` / `QueueConflictHostEdit` / `LeaveConflictHost` and `FakeNetworkSwitch`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A: nothing is extracted. `SmatchetOfflineQueueUi.cpp` is over the 1,200-line `tu-line-ceiling` WARN already; this adds about 50 lines.

## UX Pillar callouts

- **Pillar 1**: one `std::set` lookup per offline row per frame, only while a discard is held (the filter is skipped when the set is empty).
- **Pillar 2**: the delete is the same UI-thread SQLite delete as today, moved to the toast's settle (or shutdown).
- **Pillar 3**: the commit captures `AppController*`. That is safe because `~AppController` settles pending undos (`SettleUserWorkBeforeShutdown`) before its members die. The cache is held as a `weak_ptr`, never as an owning pointer.
- **Pillar 4**: Undo is reachable by Ctrl+Z as well as the button.
- **Pillar 6**: the held row stays in the queue table, so a crash during the window keeps the edit queued (as a conflict). Nothing is cached as final, and the tracker is never called.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires; no perf scenario drives the conflict resolver.
2. **Pillar 2 static scanner**: N/A, no new sync I/O (the delete call exists today).
3. **Dispatcher drain**: N/A, not touched.
4. **Visible-cue bucket-E harness**: N/A, no new stall path.
5. **Marker inventory**: N/A, no markers.

## Risks / non-goals

- **Risk: a held row replays.** It cannot. It is a conflict row, which replay skips, and the resolve that clears the mark is unreachable while the row is hidden. If a future path clears the mark without the panel, the row would replay and the commit would delete an id already gone (a no-op that logs).
- **Risk: other surfaces still count the held row for up to 8 s** (status-bar offline count, grid pending markers). Accepted: the window is short and Undo is the point of it.
- **Non-goal**: undo for the queue panel's row Delete and bulk delete (non-conflict rows replay), which keep their confirm.

## Implementation handoff

N/A: single slice in `Source/Core/src/Ui/` (offline queue UI; `offline-sync` scope for the queue semantics, which are unchanged).

## Verification

- **Bucket E** (`OfflineConflictModalPanes/ConflictModal_DiscardMyEditUndo`): queue an edit, open the resolver on it as `unverified` with the tracker held down, click "Discard my edit".
  - The resolver closes, one undoable "Edit discarded" toast is live, the row is held (seam count 1) and still in `GetPendingFieldEdits`.
  - `UndoNewest`: the row is no longer held and is still queued.
  - Discard again, then `DismissAllLive` (which commits): the row is gone from `GetPendingFieldEdits`.
- **Mutation checks**: the case fails with the commit's delete removed, and with the held-set insert removed.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the coordinator session fixed the scope (#116 undo). The terms (held, settle, commit) follow `Ui/SmatchetToast.h` and the drafts-undo plan; ADR-0016 (dead letters failures-only) is kept.
- **Manual residue**: none.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Undo for the offline queue panel's row Delete and bulk delete: still needs the `Sync/` archive/restore entry point (`debt/2026-11-22-drafts-undo-deferred.md`).
- fr-FR rows for the new toast strings: with the other Wave 2 French rows (`debt/2026-11-20-input-ux-w2-held-hints-and-fr-rows.md`).

## Implementation log

## Deviations from plan

## Verification (actual)

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index from the repository root: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
