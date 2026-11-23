# Plan — Input UX Wave 2 leftover: Undo on the conflict resolver's "Discard my edit"
<!-- plan-date: 2026-11-23 -->

> **Slug**: `input-ux-w2-conflict-discard-undo` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

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
- **Cache swap guard.** Row ids are global in the one local cache DB (`AUTOINCREMENT`). The commit captures a `std::weak_ptr` to the app's cache (`LookupCacheShared()`) at the discard. If the cache was recreated in between ("Recreate local cache database"), the commit only logs and deletes nothing: the old row went with the old DB, and an id in the new DB is not the one discarded.
- **Commit failure.** If `DeletePendingFieldEdits` reports `Failed > 0`, the commit logs a warning and arms the panel status "The edit could not be discarded; it is still queued." (it stays a conflict row, so nothing replays it).
- **Both buttons.** The unverified and unknown panes share one helper, `DiscardConflictEditWithUndo`, so they behave the same. The panel status line "Edit discarded." is dropped, because the toast says it.

Not done here: the audit's alternative (move Discard away from the primary buttons) is not needed once the click can be undone. Undo for the queue panel's own row Delete (non-conflict rows, which do replay) still needs the `Sync/` entry point and stays deferred.

## Interface contracts

N/A: no tracker, command, MCP, Lua or storage interface changes. `DeletePendingFieldEdits` is called as today, only later. No `UiDrawSession` field is added (the held set is file-static in the offline-queue TU, like `g_selectedOfflineRowKeys`).

## Files to modify

1. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp`: `g_conflictDiscardHeld`, `DiscardConflictEditWithUndo`, the filter after `BuildUnifiedOfflineRows`, both "Discard my edit" call sites, and the test seam definitions.
2. `Source/Core/include/Ui/SmatchetOfflineQueueActionsUi.h`: declarations of the test seams `SmatchetOfflineConflictDiscardHeldCountForTest()` and `SmatchetOfflineConflictDiscardHeldForTest(id)`, next to the existing `SmatchetOfflineConflictDraft*ForTest` seams.
3. `tests/ui/offline_conflict_modal_panes.test.cpp`: one bucket-E case in its existing registration (no registry or CMake edit).
4. `docs/plans/shipped/input-ux-w2-conflict-discard-undo.md`: this plan (moved to `shipped/` on ship), and `docs/plans/INDEX.md`.
5. `docs/self-improvement/categories/debt/2026-11-22-drafts-undo-deferred.md`: narrow the #116 bullet to the queue panel's own row Delete.

## Existing utilities reused

- `SmatchetToastManager::PushUndoable` / `UndoNewest` / `LiveUndoableCount` / `HasLive` (`Ui/SmatchetToast.h`).
- `AppController::DeletePendingFieldEdits` (audit entry included), `AppController::LookupCacheShared` (the cache identity).
- `FinishConflictModal`, `ArmOfflineQueuePanelStatus`, `PruneOfflineSelectionToLiveRows`.
- The test file's `EnterConflictHost` / `QueueConflictHostEdit` / `LeaveConflictHost` and `FakeNetworkSwitch`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A: nothing is extracted. `SmatchetOfflineQueueUi.cpp` is over the 1,200-line `tu-line-ceiling` WARN already; this adds about 50 lines.

## UX Pillar callouts

- **Pillar 1**: one hash lookup per offline row per frame, only while a discard is held (the filter is skipped when nothing is held).
- **Pillar 2**: the delete is the same UI-thread SQLite delete as today, moved to the toast's settle (or shutdown).
- **Pillar 3**: the commit captures `AppController*`. That is safe because `~AppController` settles pending undos (`SettleUserWorkBeforeShutdown`) before its members die. The cache (`LookupCacheShared()`) is held as a `weak_ptr`, never as an owning pointer.
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
- **Known Low: a recreate that fails after closing the database.** "Recreate local cache database" withdraws the cache while it runs. If it fails after the old file closed, it reopens the same, unwiped file as a new cache object. A discard that settles across that sees a different object and deletes nothing, so the edit comes back as a conflict row. No data is lost, and discarding it again works. Telling "same file reopened" apart from "new database" would need a cache generation id in `Persistence/`, which this UI-only slice does not add.
- **Non-goal**: undo for the queue panel's row Delete and bulk delete (non-conflict rows replay), which keep their confirm.

## Implementation handoff

N/A: single slice in `Source/Core/src/Ui/` (offline queue UI; `offline-sync` scope for the queue semantics, which are unchanged).

## Verification

- **Bucket E** (`OfflineConflictModalPanes/Unverified_DiscardMyEditUndo`): queue an edit, mark it a conflict, open the resolver on it as `unverified` with the tracker held down, click "Discard my edit".
  - The resolver closes, one undoable "Edit discarded" toast is live, the row is held, gone from the panel, and still in `GetPendingFieldEdits`.
  - `UndoNewest`: the row is no longer held, shows in the panel again and is still queued.
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

- `SmatchetOfflineQueueUi.cpp`: the held set is a map from row id to `HeldConflictDiscard` (state + the `weak_ptr` cache taken at the discard). `SettleHeldConflictDiscard` runs from the toast's commit; `DropHeldConflictDiscards(app, d, rows)` runs after `BuildUnifiedOfflineRows` and retries waiting entries, forgets deleted ones, then hides the held rows. Both "Discard my edit" buttons call `DiscardConflictEditWithUndo`.
- `SmatchetOfflineQueueActionsUi.h`: the two test seams.
- `offline_conflict_modal_panes.test.cpp`: `CaseDiscardMyEditUndo`, registered as `Unverified_DiscardMyEditUndo`.

## Deviations from plan

- **Review (code-review agent, 0 critical/high/medium, 3 low), all fixed:**
  1. The commit released the hold before the published queue snapshot dropped the deleted row, so the row could flash back for a frame or two. The entry now stays in a `Deleted` state until the snapshot no longer has the row.
  2. A toast that settled while a cache recreate had the cache withdrawn dropped the delete, and a recreate that failed then put the same cache back, so the edit came back. The entry now waits (`AwaitingCache`) and is retried on the next panel draw. The related failed-recreate case that reopens the same file as a new object is kept as a known Low (§ Risks).
  3. The test's hidden/back checks used the wildcard row ref. They now pin the row by its ImGui ID, and the settle step also checks the row never shows between the delete and the snapshot.
- **Review re-check (1 low), fixed:** a `Deleted` entry was forgotten only once the snapshot lost its id. After a successful recreate the new database numbers rows from 1 again, so an unrelated queued edit with the same id could stay hidden. A `Deleted` entry is now also forgotten when the cache is a different object than at the discard.
- The held set is an `unordered_map` with a state, not the planned `unordered_set` (from fixes 1 and 2).
- `DrawUnifiedOfflineQueuesPanel` now builds the rows and runs `DropHeldConflictDiscards` before its empty-queue early return. Otherwise a discard that deleted the last queued row was never forgotten (found by the new test).

## Verification (actual)

- **Bucket E**: `OfflineConflictModalPanes` with the Jira fixture and the local cache (`SMATCHET_UITEST_WITH_LOCAL_CACHE=1`, `test-ui-jira-deterministic-backend.sh`): 17/17, no skips, `Unverified_DiscardMyEditUndo` included. `OfflineConflictModalPanes,OfflineFirst` on the default driver: 55/55 (the conflict cases skip there without the fixture).
- **Mutation checks**: each failed the new case (16/1): the commit's delete removed, the held-set insert removed (both on the first revision), and the `Deleted` state replaced by an immediate erase (the row flashes back).
- **Build gate**: `ninja-iter-msvc` `SmatchetStandalone` + `SmatchetCore_DX12` clean.
- **Not covered by a test**: the `AwaitingCache` retry (a toast settling mid-recreate). No test seam holds a recreate open; the path is reviewed only.
