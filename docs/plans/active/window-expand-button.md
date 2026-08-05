# Plan — Window Expand Button

> **Slug**: `window-expand-button`
>
> **Status**: `active`

## Context

Users working with multiple panes in the grid view need a quick way to expand a pane to fill the available viewport space without losing their dock layout. Currently, users can only close panes or rearrange them via ImGui docking, but there's no "expand/focus" button that temporarily maximizes a pane while preserving the ability to restore the previous layout.

This feature adds an **Expand button** to the left of the close (X) button in each pane window's title bar. Clicking it will expand the selected pane to occupy the full viewport, and clicking it again (or pressing Escape) will restore the previous docked layout.

**Intended outcome**: After this lands, users can quickly toggle between a multi-pane overview and a single-pane focused work mode with one click.

Cross-link: N/A — new feature request.

## Approach

The implementation leverages ImGui's existing window flags and the pane-window infrastructure already in place:

1. Add a new UI state field to track which pane (if any) is expanded (`expandedPaneId` in `UiDrawSession`)
2. In `drawActiveProjectWindow`, add an expand button using `ImGui::ImageButton` or custom rendering positioned before the close button area
3. When expand is clicked on a pane:
   - Store the current dock node ID/layout state
   - Set that pane as the expanded pane
   - Use `ImGuiWindowFlags_NoDocking` or undock the window to allow full-viewport expansion
4. When a pane is expanded, hide other panes from rendering (but keep them alive for context sync)
5. Provide a collapse mechanism (click expand button again, or Escape key) to restore the previous layout

The key insight is that we don't need to actually move windows — we control visibility and can use ImGui's docking system strategically. The expand button will be rendered in the title bar area using ImGui's custom title bar rendering or by placing it adjacent to the window controls.

**Trade-off**: We're choosing to hide other panes rather than physically resize dock nodes because it's simpler, preserves the user's exact layout, and avoids complex dock-node manipulation that could break across ImGui versions.

## Files to modify

1. `Source/Core/include/Ui/SmatchetUI.h`:line ~400 — Add `std::string expandedPaneId;` to `UiDrawSession` struct (or nearby state container)
2. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp`:line 138 — Modify `drawActiveProjectWindow` to:
   - Render expand button in title bar (before close button)
   - Handle expand/collapse click logic
   - Skip rendering non-expanded panes when one pane is expanded
3. `Source/Core/src/Ui/SmatchetGridPaneWindows.cpp`:line 143 — Modify the pane loop to skip drawing non-expanded panes when `d.expandedPaneId` is set
4. `Source/Core/include/Ui/SmatchetGridPaneWindows.h`:line ~50 — May need to expose helper for expand state management (TBD)
5. `Source/Core/src/Ui/SmatchetUI.cpp`:line ~1047 — May need to handle global Escape key to collapse expanded pane

## Existing utilities reused

- `SmatchetIconButtonLabel` / `SmatchetIconButtons.h` — for consistent button styling
- `SmatchetLocalizedImGui` — for localized tooltips
- `SmatchetToastManager` — optional: toast notification when expanding/collapsing
- `prepareTopLevelWindow` / `repairTopLevelWindow` — existing window setup helpers

## Extraction sizing

N/A — this plan does not extract or split code; it modifies existing functions inline within size limits.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: No impact — expand state is a simple string comparison per pane; hiding panes reduces render load.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: No impact — all operations are immediate state changes.
- **Pillar 3 (never crash)**: Must handle edge cases: expanded pane closed while expanded, backend switch while expanded. Mitigation: auto-collapse on pane close.
- **Pillar 4 (accessibility)**: Button needs keyboard accessibility (tab stop, Enter/Space activation), tooltip text, and high-contrast icon. Follow-up: ensure focus order places expand button logically.

## Perf-review-system gates

N/A — diff touches UI rendering path but doesn't introduce sync I/O, new allocations in hot paths, or marker changes. Standard CI perf checks apply.

## Risks / non-goals

**Risks**:
- Dock layout restoration might not be pixel-perfect if ImGui's .ini state changes during expand
- Multiple monitors: full-viewport behavior needs definition (current monitor vs all)
- Mobile mode: expand button may need different placement or be hidden

**Mitigations**:
- Test dock restore after expand→close→collapse sequence
- Constrain expand to current ImGui viewport (default behavior)
- Gate expand button behind `!embedded` flag for mobile

**Non-goals**:
- This plan does NOT implement a separate "zen mode" or hide the menu bar
- This plan does NOT persist expanded state across sessions
- This plan does NOT animate the expand/collapse transition

## Verification

- **Bucket A (pure-logic ctest)**: N/A — pure UI interaction, no algorithmic logic to unit test
- **Bucket E (ImGui Test Engine)**: Add scenario `window_expand_collapse` that:
  1. Creates 2 panes
  2. Clicks expand on pane 1
  3. Verifies pane 2 is not visible
  4. Clicks expand again (or sends Escape)
  5. Verifies both panes visible again
- **Bash-driver scenario**: Manual verification script capturing screenshots before/after expand
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`
- **Doc validation**: Run `scripts/dev/test-docs.sh`
- **Plan stress-test**: Run `grill-with-docs` against this plan

## Out of scope (flagged, not designed)

- Persisting expanded state to `smatchet_panes.json`
- Keyboard shortcut (e.g., F11) to toggle expand
- Expand animation / smooth transition
- Multi-monitor aware expansion
- Mobile/tablet touch gesture alternative

## Implementation log

*(populated post-ship)*

## Deviations from plan

*(populated post-ship)*

## Verification (actual)

*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)

1. Flip the § Status header to `shipped`
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`
3. Regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`

*(Delete this `## Archive` block as part of step 2)*
