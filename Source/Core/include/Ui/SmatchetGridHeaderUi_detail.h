#pragma once

// Pure, ImGui-free decision helpers extracted from DrawGridHeaderToolbar so the chip-fade
// math and right-cluster width accumulation are bucket-A testable and the draw body stays
// under the function-size branch cap. No ImGui, no global state — referentially transparent.

namespace SmatchetGridHeader {
namespace detail {

/** Fraction (0..1) of a fade window elapsed, given elapsed and total seconds. Clamped to the
 *  closed unit interval; a non-positive duration returns 1 (fully faded). */
float ChipFadeFraction(float elapsedSec, float durationSec);

/** The flag + measured-width inputs needed to lay out the right-aligned header chip cluster.
 *  Mirrors the visible-chip set; all widths are pre-measured pixel extents. */
struct RightClusterWidths {
    bool showTrackerChip = false;
    float trackerW = 0.0f;
    bool showReadOnlyChip = false;
    float readOnlyW = 0.0f;
    bool showMcpChip = false;
    float mcpW = 0.0f;
    float newIssueBtnW = 0.0f;
    float betweenSpacing = 0.0f;
};

/** Total pixel width of the right-aligned cluster, inserting betweenSpacing between adjacent
 *  visible elements. The "+ New Issue" button is included unless readOnlyMode is set. */
float AccumulateRightWidth(const RightClusterWidths& w, bool readOnlyMode);

} // namespace detail
} // namespace SmatchetGridHeader
