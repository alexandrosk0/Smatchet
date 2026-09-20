# Plan — Drag-to-paint checkbox lists

> **Slug**: `drag-to-paint-checkbox-lists`
>
> **Status**: `active`

## Context

Ticking fields in **Views > Fields** is one click per row. A view that needs twenty fields costs
twenty aimed clicks, and clearing one costs twenty more — the same friction exists in every other
checkbox LIST in the app (the multi-select / components cell editor, the Labels cell editor). The
user asked for the gesture every file manager and spreadsheet has: press one checkbox and drag
across its neighbours to set them all.

After this lands: pressing a checkbox in any of those lists and dragging over the rows below (or
above) sets every row the pointer crosses to the value the press produced — drag from a checked row
to clear a run, from an unchecked row to set one — while a plain click, keyboard Space, and every
disabled row behave exactly as they do today.

## Approach

One widget, `SmatchetDragCheckbox(label, &value, flags)`, drop-in for `ImGui::Checkbox` (same
`if (...) { }` call shape, same widget id, same localization of the label), backed by a pure
state machine in `SmatchetDragCheckboxPure.h`. It ships on the Views > Fields list; the two grid
cell editors are a non-goal for the reason in § Risks / non-goals.

The gesture inverts Dear ImGui's own timing, which is what makes it work: `ImGui::Checkbox` toggles
on *release*, so a press-drag-release that ends on a different row would never toggle the row it
started on. The widget therefore applies the toggle on the **press** (opening the run and recording
the value it paints), paints every other row the pointer sweeps, and — when the release lands back
on the origin row and ImGui fires its own toggle — restores the painted value so the release is not
a second toggle. That restore is why the gesture outlives the button: the release frame already
reads button-up, so a run that ended at button-up would leave ImGui's toggle to undo the press and
turn every plain click into a no-op reported twice. Four properties keep the gesture from doing anything the user did not ask for: the
run is scoped to the window/child the press started in; disabled rows are inert; the swept pointer
path is hit-tested (a fast flick leaves no holes); and a held-still pointer never paints, so a list
that re-flows mid-gesture cannot cascade the run onto whatever row slides under the cursor.

The header lives at the `include/` root rather than `include/Ui/` for the same reason as
`TouchCellEditGesture.h`: two of its consumers are domain-side cell-editor TUs, and domain code must
not include `Ui/` headers (`no-ui-include-in-domain`). It is header-only (the gesture's single
instance is a function-local static), so there is no new TU and no link edge from domain code into
`Ui/`.

## Files to modify

1. `Source/Core/include/SmatchetDragCheckboxPure.h` (new) — ImGui-free gesture state machine:
   `GestureLapsed`, `SegmentCrossesBand`, `DecideItem`.
2. `Source/Core/include/SmatchetDragCheckbox.h` (new) — the ImGui widget: live hit-testing, the
   disabled/scope probes, and the write-back that drives the pure decisions.
3. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:514,565` — the Views > Fields System/Custom
   groups and the Basic-fields group (the list the request named).
4. `tests/Core/SmatchetDragCheckboxPure.test.cpp` (new) + `tests/CMakeLists.txt` — bucket-A coverage,
   registered in both the full rig and the Linux/TSan curated subset.
5. `tests/ui/drag_checkbox_paint.test.cpp` (new) + `tests/ui/CMakeLists.txt` +
   `tests/ui/ui_tests_registry.cpp` — bucket-E coverage driving the real widget.

## Existing utilities reused

- `SmatchetLocalization::LabelFromSource` · `Source/Core/src/SmatchetLocalization.cpp:1635` — the
  exact transform `SmatchetLocalizedImGui::Checkbox` applies, so swapping the call keeps each
  widget's visible text and its ImGui id in every locale.
- `ImRect::ClipWith` / `ImGui::IsMousePosValid` / `ImGuiIO::MousePosPrev` — the swept-path hit test
  needs no new geometry helper.
- `TouchCellEditGesture.h` · `Source/Core/include/TouchCellEditGesture.h:1` — the precedent for an
  ImGui gesture header at the `include/` root consumed by domain cell-editor TUs.

## Extraction sizing

N/A — this plan adds a widget, it extracts nothing.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: per-row cost is a handful of float compares and one
  `IsWindowHovered` call, and only while a gesture is in flight (`gesture.Active` short-circuits the
  hit test). No allocation, no per-frame container work.
- **Pillar 2 (UI-thread never blocks)**: no I/O, no locks, no async — pure input arithmetic.
- **Pillar 3 (never crash)**: no raw allocation or pointer ownership; the one piece of mutable state
  is a function-local static touched only from the UI thread; `IM_ASSERT` guards the null contract;
  every ImGui probe runs after the widget call that establishes `LastItemData`, and the
  `SkipItems` early-out keeps a clipped window from reading another item's `LastItemData`.
- **Pillar 4 (accessibility)**: keyboard activation is untouched — a Space toggle takes ImGui's own
  path and is reported unchanged. The gesture is an accelerator, never the only way to tick a row.

## Perf-review-system gates

1. **PR-fast CI** — closest scenario: the Views-editor scenario that renders the Fields tab; the
   changed path is per-row widget code inside an already-rendered list, adding no new draw work.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — untouched.
4. **Visible-cue bucket-E harness** — no new sync-stall path.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers.

## Risks / non-goals

- **Risk: a run painting rows the user did not mean to hit.** Mitigated by the four properties in
  § Approach (scope, disabled-skip, swept path, motion-gated paint) and pinned by bucket-A tests.
- **Risk: double-toggle on the origin row** (press applies, ImGui's release re-applies). The
  release-restore branch handles it and has its own test.
- **Risk: the header included after a TU's `#define ImGui SmatchetLocalizedImGui`** would rewrite
  the header's own `ImGui::` calls and double-localize. A `#error` guard makes that a compile
  failure rather than a runtime oddity.
- **Non-goal: the grid cell editors** — the multi-select / components list
  (`TicketFieldEditor.cpp`) and the label-suggestion list (`TrackerLabelsEditor.cpp`). Both were
  wired up first and then reverted: their commit pipeline is structurally hostile to a run. Each
  toggle queues a set-REPLACE edit; `EnqueueGridFieldEdits` keeps only the latest queued edit per
  cell; and `PumpGridFieldEdits` immediately marks the cell `CellWriteState::Saving`, which makes
  `SmatchetActiveProjectGridCells.cpp` render the read-only cell instead of `RenderFieldCell` — so
  the combo closes on the first queued edit and the gesture dies mid-drag. A run there would only
  ever cover the rows swept in one frame, while still changing the press-to-commit semantics of
  those editors. Making them work needs the commit deferred until the editor closes, which is the
  tracker write path, not this feature. Backlogged.
- **Non-goal: the mobile-nav hidden-pages list** (`SmatchetPreferencesUi_Local.cpp:205`). Its rows
  leave the list the moment they are ticked, so a drag has no stable run to paint and could add nav
  pages the user never aimed at. Left on plain `ImGui::Checkbox`.
- **Non-goal: shift-click range selection.** A different gesture (anchor + extend), not asked for.
- **Non-goal: drag-select for the `Selectable`-based lists** (column order, view list) — those own
  drag already (row reorder).

## Verification

- **Bucket A (pure-logic ctest)**: `tests/Core/SmatchetDragCheckboxPure.test.cpp` — press opens the
  run and writes the toggled value (both directions), a neighbour paints once and only when it
  differs, the origin release is not a second toggle, an unowned toggle is reported not rewritten,
  disabled rows neither open a run nor take paint, a run never crosses into another list, a press
  never hijacks a run in flight, `GestureLapsed` drops a run on button-up or a frame gap, and the
  swept band covers a fast flick.
- **Bucket E (ImGui Test Engine)**: `tests/ui/drag_checkbox_paint.test.cpp` — drives the real widget
  under the engine: press row 0, drag to row 4, release; asserts the crossed rows flipped, the
  disabled row in the middle did not, and the row past the release point did not.
- **Build gate**: `SmatchetCore_PosixCheck` (compiles every Core TU incl. the three call sites) and
  the `SmatchetTsanTests` Linux subset.
- **Manual residue**: the feel of the gesture (latency, how far a drag may stray off the row) is
  judged by the user on a running build — the Pillar-4 visual-validation exception.

## Out of scope (flagged, not designed)

- Touch/pen drag-paint on the mobile build — the same widget would need the long-press-vs-scroll
  disambiguation `TouchCellEditGesture` owns; no action this slice.
- A "select all matching the filter" button per group — the Fields tab already has
  *Select all visible* / *Clear visible*.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
