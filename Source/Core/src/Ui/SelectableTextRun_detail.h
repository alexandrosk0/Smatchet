#ifndef SMATCHET_SELECTABLE_TEXT_RUN_DETAIL_H
#define SMATCHET_SELECTABLE_TEXT_RUN_DETAIL_H

// SelectableTextRun pure-geometry / selection-math helpers.
//
// These free functions carry NO ImGui-context dependency (they touch only
// ImVec2 PODs and the exposed Segment vector) so the bucket-A doctest rig can
// exercise the bounding-box, hit-containment, and per-segment selection-span
// math without an ImGui context. SelectableTextRun.cpp's End() composes them so
// the ImGui-emit half stays thin and under the function-size branch cap.

#include "SelectableTextRun.h"

#include "imgui.h"

#include <cstddef>
#include <vector>

namespace SelectableText {
namespace detail {

// Axis-aligned bounding box result for the registered segments.
struct BoundingBox {
    ImVec2 min = ImVec2(0, 0);
    ImVec2 max = ImVec2(0, 0);
};

// Per-segment [from,to) byte span selected for segment `i`, already clamped to
// [0, segLen] and ordered (from <= to).
struct SegmentSpan {
    int from = 0;
    int to = 0;
};

// Compute the union bounding box across all segments: min covers the top-left
// extreme, max covers (pos + width/lineH) extreme. Caller guarantees non-empty.
BoundingBox ComputeBoundingBox(const std::vector<Segment>& segments);

// Pure point-in-segment test: true when (x,y) falls inside the segment's
// [pos, pos+width] x [pos, pos+lineH] rect.
bool PointInSegment(const Segment& s, float x, float y);

// Index of the first segment containing (x,y), or -1 if none. Mirrors the
// first-hit-wins loop in End() (no sub-glyph offset — that needs the ImFont).
int HitTestSegmentIndex(const std::vector<Segment>& segments, float x, float y);

// Per-segment selection span for segment `i` within a normalised selection
// [(aSeg,aChar) .. (bSeg,bChar)]. `segLen` is segment `i`'s byte length.
// Endpoints are clamped to [0, segLen] and ordered. Shared by the overlay-rect
// loop and GetSelectedText so both agree byte-for-byte.
SegmentSpan ComputeSegmentSpan(int i, int aSeg, int aChar, int bSeg, int bChar, int segLen);

// True when the persisted selection endpoints reference segments still present
// in `ctx` (i.e. both endpoints in [0, segCount)). Pure — mirrors the in-range
// guard that gates the overlay draw in End().
bool SelectionEndpointsInRange(const Context& ctx);

// Mutate `ctx` to select every byte across all `segCount` segments. No-op when
// `segCount <= 0`. Pure (no ImGui) — shared by the Ctrl+A and context-menu
// "Select all" paths so they stay byte-identical.
void SelectAllSegments(Context& ctx, int segCount);

} // namespace detail
} // namespace SelectableText

#endif // SMATCHET_SELECTABLE_TEXT_RUN_DETAIL_H
