# Plan — Input UX Wave 2 leftover: Undo on the view-delete toast

> **Slug**: `input-ux-w2-view-delete-undo` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

Piece 4.3 of `docs/audits/INPUT_UX_AUDIT.md` (`docs/plans/input-ux-w2-drafts-undo.md`, PR #2439) added the toast Undo (`SmatchetToastManager::PushUndoable`, Ctrl+Z) and meant to put it on the view delete (#90). The branch carried that change, then dropped it (commit `a14c917d2`) because a sibling plan-lock (`pane-picker-jira-sites`) took `SmatchetViewsDashboardUi.cpp`. That lock is gone. Bugbot's note on the dropped version still applies: the Undo must also bring the grid back to the restored view.

Intended outcome: after a view is deleted (through its confirm), the "View deleted" toast has an Undo button and Ctrl+Z, which put the view back with its id, so every grid pane that named it finds it again.

## Approach

Undo re-creates the captured `ViewDefinition` through a new one-frame restore latch on `UiDrawSession`, consumed at the top of `SmatchetUI::applyPendingViewDelete` (it already runs at the top of `Draw`, takes `AppController&`, and sits right after the create latch). The store is never resized mid-frame (`Source/Core/src/Ui/AGENTS.md`, the view latch rule).

- `Views::Create` keeps the captured id unless it is taken, appends the view and makes it active.
- If the deleted view was the active one, the restored view stays active and the editor buffers, `cfg.JqlQuery` / `cfg.SelectedFields` and the grid follow it (`SyncWithCurrentView`), as the delete did for its neighbour.
- If the deleted view was not the active one, the view that is active when Undo runs is activated again, so Undo of a right-click delete does not switch the user's view.
- Undo is refused with a warning toast when the focused tracker changed since the delete: views belong to the tracker they were deleted from.

The confirm stays. The audit's "Undo instead of a confirm" is not taken here: #2437 put the confirm on the dialog contract and its tests assert it, the same call #2439 made for the chat clear.

The latch is single-slot and last-wins, like the create latch. Two Undo runs in one frame cannot happen: the button and Ctrl+Z each undo one toast per frame.

## Interface contracts

N/A: no tracker, command, MCP, Lua or storage interface changes. `UiDrawSession` gains three UI-thread fields (below). The views file format is unchanged; the restore writes through `Views::Create` and its `Save()`.

## Files to modify

1. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp`: `PushViewDeletedUndoToast` (new, file-static); `applyPendingViewDelete` captures the doomed view before the erase, consumes the restore latch first, and pushes the undoable toast.
2. `Source/Core/include/Ui/SmatchetUiSession.h`: `viewsPendingRestore`, `viewsPendingRestorePayload`, `viewsPendingRestoreKeepActiveId` next to the delete latch.
3. `tests/ui/mobile_views_confirm_modal.test.cpp`: two bucket-E cases in its existing registration function (no registry or CMake edit).
4. `docs/plans/active/input-ux-w2-view-delete-undo.md`: this plan.

Not touched: `SmatchetUI.h`, `docs/guides/keyboard-shortcuts.md`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp` and `tests/CMakeLists.txt` are in the `input-ux-w3-grid-s1` lock.

## Existing utilities reused

- `SmatchetToastManager::PushUndoable` / `UndoNewest` / `LiveUndoableCount` / `HasLive` (`Ui/SmatchetToast.h`).
- `Views::Create`, `Views::Activate`, `Views::Find` (`Ui/Views.h`).
- `LoadBuffersFromView`, `SmatchetViewsDashboardUiDetail::SyncWithCurrentView` (the delete path's own re-home).
- `AppController::FocusedCacheBackendKey` (tracker check).
- `MobileViewsModalSeam` (the test file's fresh Plane workspace).

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

- **Risk: the restored view's id is taken by then** (a new view of the same name). `Views::Create` makes it unique; panes that named the old id then show the active view. Accepted.
- **Risk: Undo after a tracker switch writes the view into the wrong tracker's store.** Refused with a warning toast.
- **Non-goal**: restoring the view at its old position in the list; it comes back last, as a created view does.
- **Non-goal**: Undo for the mobile and command (`view.delete`) delete paths, which do not go through this latch.

## Implementation handoff

N/A: single slice, `Source/Core/src/Ui/` (grid-engine scope: `SmatchetViewsDashboardUi*`).

## Verification

- **Bucket A**: N/A, no new pure logic; the store behaviour is `Views::Create`'s, already covered by `tests/Core/ViewsPendingCreate.test.cpp`.
- **Bucket E**: `ViewsDeleteUndo/UndoBringsBackActiveView` (delete the active view, Undo, the view is back, active and loaded in the editor) and `ViewsDeleteUndo/UndoKeepsCurrentViewForNonActiveDelete` (delete a non-active view, Undo, it is back and the active view did not change). Mutation check: each fails with the undo callback emptied.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: the coordinator session fixed the scope (one function plus a call); the terms here are the drafts-undo plan's.
- **Manual residue**: none.

## Open questions

None.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- The keyboard guide sentence listing what Undo covers (`docs/guides/keyboard-shortcuts.md`, "A confirmed action can be taken back…") gains "deleting a view" once the `input-ux-w3-grid-s1` lock on that file is released.
- fr-FR rows for the new toast strings: with the other Wave 2 French rows (`debt/2026-11-20-input-ux-w2-held-hints-and-fr-rows.md`).

## Implementation log

## Deviations from plan

## Verification (actual)

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index from the repository root: `bash agent-layer/agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
