# Plan — Input UX Wave 2 leftover: Undo on the view-delete toast
<!-- plan-date: 2026-11-23 -->

> **Slug**: `input-ux-w2-view-delete-undo` (matches this file's basename without `.md`).
>
> **Status**: `shipped` (one PR on `claude/input-ux-w2-view-delete-undo`; code, tests and this plan land together).

## Context

Piece 4.3 of `docs/audits/INPUT_UX_AUDIT.md` (`docs/plans/input-ux-w2-drafts-undo.md`, PR #2439) added the toast Undo (`SmatchetToastManager::PushUndoable`, Ctrl+Z) and meant to put it on the view delete (#90). The branch carried that change, then dropped it (commit `a14c917d2`) because a sibling plan-lock (`pane-picker-jira-sites`) took `SmatchetViewsDashboardUi.cpp`. That lock is gone. Bugbot's note on the dropped version still applies: the Undo must also bring the grid back to the restored view.

Intended outcome: after a view is deleted (through its confirm), the "View deleted" toast has an Undo button and Ctrl+Z, which put the view back with its id, so every grid pane that named it finds it again.

## Approach

Undo re-creates the captured `ViewDefinition` through a new one-frame restore latch on `UiDrawSession`, consumed at the top of `SmatchetUI::applyPendingViewDelete` (it already runs at the top of `Draw`, takes `AppController&`, and sits right after the create latch). The store is never resized mid-frame (`Source/Core/src/Ui/AGENTS.md`, the view latch rule).

- `Views::Create` keeps the captured id unless it is taken, appends the view and makes it active.
- The active view is the focused pane's view. If the pane focused when Undo runs is one that showed the deleted view (`RestoredViewTakesFocus`), the restored view stays active and the editor buffers, `cfg.JqlQuery` / `cfg.SelectedFields` and the grid follow it (`SyncWithCurrentView`), as the delete did for its neighbour. It also does when the deleted view was the active one and focus is still on the pane focused at the delete (in mobile mode the focused pane only follows the active view while the grid draws). With no focused pane, whether the deleted view was the active one decides. The exception is a Views editor holding unapplied edits of the view now active: then the user stays on that view, so the Undo never throws a draft away.
- Otherwise (a right-click delete of another view, or focus moved to a pane on another view during the Undo window) the view that is active when Undo runs is activated again, so Undo does not switch the user's view.
- The other grid panes that showed the view are bound to it again. In the frame after the delete, `SmatchetUI::resolvePaneView` moved them to the view active then, so the delete records their ids and that fallback id, and the restore rebinds the ones still on the deleted id or the fallback (never the focused pane, which follows the active view, and never a pane the user moved to another view since).
- Undo is refused with a warning toast when the views bucket (`NormalizeViewsBackendKey(cfg.TrackerType)`) changed since the delete, checked at the click and again when the latch is consumed: views belong to the tracker they were deleted from.

The confirm stays. The audit's "Undo instead of a confirm" is not taken here: #2437 put the confirm on the dialog contract and its tests assert it, the same call #2439 made for the chat clear.

The latch is single-slot and last-wins, like the create latch. Two Undo runs in one frame cannot happen: the button and Ctrl+Z each undo one toast per frame.

## Interface contracts

N/A: no tracker, command, MCP, Lua or storage interface changes. `UiDrawSession` gains seven UI-thread fields (below). The views file format is unchanged; the restore writes through `Views::Create` and its `Save()`.

## Files to modify

1. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp`: file-static `PushViewDeletedUndoToast`, `ApplyPendingViewRestore`, `RehomeOnActiveView` (the delete's re-home, now shared) and `PushViewRestoreRefusedToast`; `applyPendingViewDelete` captures the doomed view and its panes before the erase, consumes the restore latch first, and pushes the undoable toast.
2. `Source/Core/src/Ui/SmatchetViewsDashboardUi_detail.h`: pure templates `PaneIdsShowingView`, `RestoredViewTakesFocus` and `RebindPanesToRestoredView` (over a `PaneRestoreTarget`).
3. `Source/Core/include/Ui/SmatchetUiSession.h`: `viewsPendingRestore`, `viewsPendingRestorePayload`, `viewsPendingRestoreWasActive`, `viewsPendingRestoreBackendKey`, `viewsPendingRestorePaneIds`, `viewsPendingRestoreFallbackId`, `viewsPendingRestoreFocusedPaneId` next to the delete latch.
4. `tests/Core/ViewsPendingCreate.test.cpp`: bucket-A cases for the three templates and the id-keeping re-create (already in `SmatchetTests`).
5. `tests/ui/views_editor_header.test.cpp`: one bucket-E case in its existing registration function (no registry or CMake edit); it already drives `view.create` / `view.list` / `view.current`.
6. `docs/plans/shipped/input-ux-w2-view-delete-undo.md`: this plan (written under `active/`, moved on ship), and `docs/plans/INDEX.md`.
7. `docs/self-improvement/categories/debt/2026-11-22-drafts-undo-deferred.md`: drop the view-delete bullet, add the keyboard guide line.

Not touched: `SmatchetUI.h`, `docs/guides/keyboard-shortcuts.md`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp` and `tests/CMakeLists.txt` are in the `input-ux-w3-grid-s1` lock.

## Existing utilities reused

- `SmatchetToastManager::PushUndoable` / `UndoNewest` / `LiveUndoableCount` / `HasLive` (`Ui/SmatchetToast.h`).
- `Views::Create`, `Views::Activate`, `Views::Find` (`Ui/Views.h`).
- `LoadBuffersFromView`, `SmatchetViewsDashboardUiDetail::SyncWithCurrentView` (the delete path's own re-home).
- `ConfigManager::NormalizeViewsBackendKey` (tracker check), `SyncViewsEditorDraft` + `ViewDraftHasUnsavedEdits` (draft guard), `SmatchetGridPaneWindows::MarkPanesDirty` (panes save).
- `RunCommand` / `ViewIdsInOrder` / `CurrentViewId` / `ViewsDashboardStateGuard` (the test file's own helpers).

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A: nothing is extracted.

## UX Pillar callouts

- **Pillar 1**: no per-frame cost; the latch check is one bool at the top of `Draw`.
- **Pillar 2**: `Views::Create` saves the views file, as every create and delete already does on the UI thread; no new I/O class.
- **Pillar 3**: the restore runs at the top-of-`Draw` latch point, never from the toast's click handler mid-frame (the undo callback only writes the latch).
- **Pillar 4**: the Undo is reachable by Ctrl+Z as well as the button.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires; no scenario drives a view delete, so the generic UI scenarios cover the unchanged frame path.
2. **Pillar 2 static scanner**: N/A, no new sync I/O reachable from `ImGui::*` (the save is the existing `Views::Save`).
3. **Dispatcher drain**: N/A, not touched.
4. **Visible-cue bucket-E harness**: N/A, no new stall path.
5. **Marker inventory**: N/A, no markers.

## Risks / non-goals

- **Risk: the restored view's id is taken by then** (a new view of the same name). `Views::Create` makes it unique, and the panes are rebound to the new id.
- **Risk: Undo after a tracker switch writes the view into the wrong tracker's store.** Refused with a warning toast.
- **Non-goal**: restoring the view at its old position in the list; it comes back last, as a created view does.
- **Non-goal**: Undo for the mobile and command (`view.delete`) delete paths, which do not go through this latch.

## Implementation handoff

N/A: single slice, `Source/Core/src/Ui/` (grid-engine scope: `SmatchetViewsDashboardUi*`).

## Verification

- **Bucket A**: `tests/Core/ViewsPendingCreate.test.cpp`: `PaneIdsShowingView` (bound panes listed, empty id lists none), `RebindPanesToRestoredView` (listed same-tracker panes on the deleted id or the fallback rebound; focused, other-tracker, unlisted and since-moved panes untouched; a second run changes nothing), `RestoredViewTakesFocus` (focused pane that showed it, focus moved elsewhere, no focused pane) and the delete-then-re-create round trip keeping the id.
- **Bucket E**: `ViewsEditorHeader/DeleteUndo_BringsViewBackKeepsCurrentView`: delete the active view through the delete latch, Undo, the view is back under its id and active, with a "View restored" toast; the editor draft and `cfg.JqlQuery` follow it (B has its own query); then delete a non-active view, Undo, it is back and the active view did not change. The "View restored" check reads the toast history, which is not time-bound. Mutation checks: it fails with the undo callback emptied, with the keep-active step removed, and with the active-view re-home removed.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the coordinator session fixed the scope (one function plus a call); the terms here are the drafts-undo plan's.
- **Manual residue**: none.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- The keyboard guide sentence listing what Undo covers (`docs/guides/keyboard-shortcuts.md`, "A confirmed action can be taken back…") gains "deleting a view" once the `input-ux-w3-grid-s1` lock on that file is released; tracked in `docs/self-improvement/categories/debt/2026-11-22-drafts-undo-deferred.md`.
- fr-FR rows for the new toast strings: with the other Wave 2 French rows (`debt/2026-11-20-input-ux-w2-held-hints-and-fr-rows.md`).

## Implementation log

- `wip(plan)` commit `10a090058`: this plan, write set claimed under `refs/locks/input-ux-w2-view-delete-undo`.
- Feature commit (same branch): the restore latch on `UiDrawSession`, the undoable "View deleted" toast and `ApplyPendingViewRestore` in `SmatchetViewsDashboardUi.cpp`, the two pane templates in `SmatchetViewsDashboardUi_detail.h`, three bucket-A cases and one bucket-E case; this plan flipped to `shipped` and moved.

## Deviations from plan

- **Review round 1 (code-review agent: 1 high, 4 medium, 5 low; all fixed).** The first version left the other grid panes on the fallback view (high; now rebound), checked `FocusedCacheBackendKey` once at the click instead of the views bucket at click and consume (medium), could overwrite unapplied editor edits on an active-view Undo (medium; now stays put), checked the "View restored" toast against its 1.8 s life (medium; now the history), and did not assert that the editor and grid query follow the restored view (medium). Lows: the callback no longer captures the `Views` store, the re-home block is shared with the delete, the new branches log, and the test clears every latch field.
- **Review round 2 (code-review agent: 1 medium, 2 low; all fixed).** Undo decided "make the restored view active" from whether it was active at delete time, so a focus switch to another pane during the Undo window let Undo take that pane over (medium; now `RestoredViewTakesFocus` asks the pane focused at Undo time, which also covers the mirror case). The rebind pulled back a pane the user had moved to another view since (low; now only panes on the deleted id or the fallback). A failed bucket-E check could leave the undoable toast live for a later test (low; the teardown dismisses live toasts). The re-check found one new low in the focus fix: in mobile mode the focused pane may not have followed the active view, so `paneIds` misses it; the pane focused at the delete is now captured too.
- **Pane rebind is bucket A, not bucket E.** A bucket-E case would have to add a live grid pane, which starts its own sync; the rebind is a pure template over the pane list instead, tested with plain structs.

## Verification (actual)

- **Build gate**: `ninja-iter-msvc` `SmatchetStandalone` + `SmatchetCore_DX12` built clean.
- **Bucket A**: the four new `SmatchetTests` cases pass; the full suite passes (see the PR's run).
- **Bucket E**: `UI_TEST_FILTER="ViewsEditorHeader,MobileViewsConfirmModal,DialogContract,Views,GridPanes"`: 24 passed, 0 failed, including `DeleteUndo_BringsViewBackKeepsCurrentView`.
- **Mutation checks**: each of the three mutants (undo callback emptied, keep-active step removed, active-view re-home removed) made `ViewsEditorHeader` fail (passed=3 failed=1); the source was restored and rebuilt clean after each.
- **Lint**: `test-lint-rules.sh --diff origin/develop` passed on the final diff (exit 0); `scripts/dev/test-docs.sh` passed after the plan's own path was moved to `shipped/`.
