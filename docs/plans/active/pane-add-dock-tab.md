# Plan — "+" opens the new pane as a tab next to its source

**Status**: implemented, visually verified by the user (2026-08-16)
**Branch**: `feat/pane-add-dock-tab`
**Reported symptom**: "when I press + to open a new pane, it doesn't open in the tabs"

## Problem

Clicking the pane strip's `+` creates a new `GridPane`, but the new window did not
join the tab bar of the pane whose `+` was clicked. It either landed in the static
default grid dock node, floated, or (most commonly) docked correctly yet stayed
*behind* its sibling's tab — reading to the user as "`+` did nothing".

Root causes, in the order they bite:

1. **No placement hand-off.** `ApplyPaneAddAndCloseRequestsCore` copied identity
   (title / backend / view / snapshot) from the source pane but nothing about where
   the source window lives, so the new window had no node to dock into.
2. **Shared layout key across N pane windows.** Every pane called
   `prepareTopLevelWindow(d, "active", …)` / `repairTopLevelWindow(d, "active", …)`.
   `d.pendingReDockWindows` is keyed by that string: the first pane in the loop
   consumes the arm and a later floating pane re-inserts it — N windows fighting one
   latch written for exactly one window. The static `"active"` dock slot also
   scattered new panes into the default grid node.
3. **Docking does not select the tab** (upstream imgui #2304 — already documented in
   `SmatchetUI::selectDockedTab`). Even a correctly docked new pane opens behind its
   sibling. This is the literal reported symptom.

Not a cause: a stale hardcoded dock slot id. `SmatchetDockNodeIds::EnsureDockSlotAlive`
already collapses a dead/orphan node to 0, so `prepareTopLevelWindow` skips the
force-dock rather than docking into a detached node.

## Approach

A runtime-only, three-step hand-off. Dock **geometry** keeps riding ImGui's `.ini`
(ADR-0018) — nothing here is persisted to `smatchet_panes.json`.

- `GridPane::lastDockId` — refreshed from `ImGui::GetWindowDockID()` right after every
  `Begin`, including the clipped frames that return `false` (a pane behind a tab is
  still docked, and is exactly the pane a later `+` hands off from).
- `GridPane::pendingDockId` — seeded at creation from the source's `lastDockId`,
  consumed **once** by the new pane's first `Begin` via
  `SetNextWindowDockID(..., ImGuiCond_Always)`. `Always` deliberately overrides a stale
  `[Window][GridPane:pane-N]` ini entry, which also repairs already-degraded layouts.
  After the consume the user owns the placement (re-forcing would fight a tab drag).
- `GridPane::selectTabFrames` — counted down in the draw loop, calling the new
  `SmatchetUI::selectCurrentDockedTab()`. Several frames because the node's tab bar
  does not exist yet on the frame the window first docks.

A floating source (`lastDockId == 0`) hands off nothing: ImGui cannot tab a window
into a node that does not exist, so the new pane falls back to first-use placement.

Extra panes get their own pre-Begin path (`PrepareExtraPaneWindow`) and an
unregistered `"grid-pane"` repair key, so they never touch the bootstrap pane's
`"active"` latch; the unregistered key routes `repairTopLevelWindow` to its
rect-repair-only branch (off-screen / degenerate rescue kept, no re-dock arm).

## Files modified

| File | Change |
|---|---|
| `Source/Core/include/GridPane.h` | `lastDockId` / `pendingDockId` / `selectTabFrames` runtime fields (plain `unsigned int` — header stays ImGui-free) |
| `Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp` | seed the hand-off on `+` (pure core, still ImGui-free) |
| `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp` | `PrepareExtraPaneWindow` helper, per-pane `lastDockId` capture, tab-select countdown, `"grid-pane"` repair key |
| `Source/Core/src/Ui/SmatchetUI_Layout.cpp` | `selectCurrentDockedTab()` — `selectDockedTab` for the window currently between `Begin`/`End` (a pane's name carries a `###GridPane:<id>` settings id the by-name lookup is not meant to see) |
| `Source/Core/include/Ui/SmatchetUI.h` | declare it |
| `tests/Core/GridPaneRequests.test.cpp` | two bucket-A cases: docked source hands off node + tab arm (source untouched); floating source hands off nothing |

## Perf gate

Touches `Source/Core/`, so per § Process rules: no steady-state cost. The added work
is one `GetWindowDockID()` per pane per frame (an accessor on the current window) and
a branch on two ints; the `SetNextWindowDockID` / `selectCurrentDockedTab` paths run
only on the ≤ 4 frames after a `+`. No allocation, no per-cell or per-row work.
Pillar 1 budget (6.94 ms) unaffected.

## Verification

- [x] `SmatchetTests.exe` — 2882 passed / 0 failed (38273 assertions), including the
      two new pane-add cases.
- [x] `agents/scripts/project/test-lint-rules.sh --diff origin/develop` — clean.
- [x] `clang-format -i` on all six files.
- [x] Standalone MSVC build green (`ninja-iter-msvc`; `ninja-iter-clang` is broken
      independently — Issue #2029).
- [x] **Manual (visual-validation exception — touches `Smatchet*Ui*.cpp`, no bucket-C/E
      coverage)**: click `+` on a docked pane → new pane appears as the **selected** tab
      in the same tab bar; click `+` again from the new pane → third tab joins the same
      bar; drag a pane out to float, click its `+` → new pane appears floating (no
      crash, no jump to the default grid node); close a pane → survivors keep placement.

## Deviations

None.

## Follow-ups

- Bucket-E regression guard for the new-pane tab selection. `tests/ui/docked_tab_focus.test.cpp`
  already drives `selectDockedTab` and is the natural home; it would convert the manual
  checklist above into an automated gate.
