#ifndef SMATCHET_LAYOUT_BREAKPOINTS_H
#define SMATCHET_LAYOUT_BREAKPOINTS_H

/**
 * Shared responsive-layout breakpoints for windows that swap between a wide
 * (single-line rows) and a narrow (stacked two-line rows, taller touch
 * targets) presentation based on their own content width — not the OS window
 * size, so a narrow docked pane on desktop gets the narrow layout too
 * (docs/plans/user-info-window.md, Slice 5).
 */
namespace smatchet {
namespace ui {

/// Content width (px, pre-DPI-scale ImGui units) below which a window should
/// use its narrow/stacked row layout.
constexpr float kNarrowLayoutWidthPx = 600.0f;

} // namespace ui
} // namespace smatchet

#endif
