// SelectableTextRun pure-geometry / selection-math helpers — see
// SelectableTextRun_detail.h for the contract.

#include "SelectableTextRun_detail.h"

#include <algorithm>
#include <utility>

namespace SelectableText {
namespace detail {

BoundingBox ComputeBoundingBox(const std::vector<Segment>& segments) {
    BoundingBox bb;
    bb.min = segments[0].pos;
    bb.max = ImVec2(bb.min.x + segments[0].totalWidth, bb.min.y + segments[0].lineH);
    for (std::size_t i = 1; i < segments.size(); ++i) {
        const Segment& s = segments[i];
        if (s.pos.x < bb.min.x) {
            bb.min.x = s.pos.x;
        }
        if (s.pos.y < bb.min.y) {
            bb.min.y = s.pos.y;
        }
        const float rx = s.pos.x + s.totalWidth;
        const float ry = s.pos.y + s.lineH;
        if (rx > bb.max.x) {
            bb.max.x = rx;
        }
        if (ry > bb.max.y) {
            bb.max.y = ry;
        }
    }
    return bb;
}

bool PointInSegment(const Segment& s, float x, float y) {
    const bool yIn = (y >= s.pos.y && y <= s.pos.y + s.lineH);
    const bool xIn = (x >= s.pos.x && x <= s.pos.x + s.totalWidth);
    return yIn && xIn;
}

int HitTestSegmentIndex(const std::vector<Segment>& segments, float x, float y) {
    for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
        if (PointInSegment(segments[i], x, y)) {
            return i;
        }
    }
    return -1;
}

SegmentSpan ComputeSegmentSpan(int i, int aSeg, int aChar, int bSeg, int bChar, int segLen) {
    SegmentSpan span;
    span.from = (i == aSeg) ? aChar : 0;
    span.to = (i == bSeg) ? bChar : segLen;
    if (span.from > span.to) {
        std::swap(span.from, span.to);
    }
    // Clamp both endpoints fully into [0, segLen] so the helper honors its
    // header contract even for unordered/out-of-range inputs (CR #736).
    if (span.from < 0) {
        span.from = 0;
    }
    if (span.from > segLen) {
        span.from = segLen;
    }
    if (span.to < 0) {
        span.to = 0;
    }
    if (span.to > segLen) {
        span.to = segLen;
    }
    return span;
}

bool SelectionEndpointsInRange(const Context& ctx) {
    const int segCount = static_cast<int>(ctx.segments.size());
    return ctx.hasSelection && ctx.selStartSeg >= 0 && ctx.selStartSeg < segCount && ctx.selEndSeg >= 0 &&
           ctx.selEndSeg < segCount;
}

void SelectAllSegments(Context& ctx, int segCount) {
    if (segCount <= 0) {
        return;
    }
    ctx.hasSelection = true;
    ctx.selStartSeg = 0;
    ctx.selStartChar = 0;
    ctx.selEndSeg = segCount - 1;
    ctx.selEndChar = static_cast<int>(ctx.segments[segCount - 1].textOwned.size());
}

} // namespace detail
} // namespace SelectableText
