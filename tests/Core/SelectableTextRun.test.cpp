// SelectableTextRun bucket-A doctest — pure-logic coverage of doc-order range
// walk + char-offset slicing + reverse-selection normalisation +
// block-boundary newline insertion.
//
// The hit-test / overlay paths depend on ImFont + an ImGui context; those live
// under bucket E (deferred). Here we construct a synthetic Context directly
// (the struct + Segment are exposed in the public header so this is clean).

#include "SelectableTextRun.h"
#include "SelectableTextRun_detail.h"

#include <doctest/doctest.h>

#include <string>

namespace {

using SelectableText::Context;
using SelectableText::Segment;

// Build a 3-segment Context across 2 blocks:
//   seg0 (block 0): "Hello"
//   seg1 (block 0): " world"   (same block as seg0 — no newline between them)
//   seg2 (block 1): "Next line" (new block — newline before this in selected output)
Context MakeFixture() {
    Context ctx;
    Segment a;
    a.docOrder = 0;
    a.blockId = 0;
    a.textOwned = "Hello";
    ctx.segments.push_back(a);
    Segment b;
    b.docOrder = 1;
    b.blockId = 0;
    b.textOwned = " world";
    ctx.segments.push_back(b);
    Segment c;
    c.docOrder = 2;
    c.blockId = 1;
    c.textOwned = "Next line";
    ctx.segments.push_back(c);
    return ctx;
}

} // namespace

TEST_CASE("SelectableText HasSelection rejects empty selection") {
    Context ctx = MakeFixture();
    // No selection at all.
    CHECK_FALSE(SelectableText::HasSelection(ctx));
    // Selection where start == end is empty.
    ctx.hasSelection = true;
    ctx.selStartSeg = 0;
    ctx.selStartChar = 2;
    ctx.selEndSeg = 0;
    ctx.selEndChar = 2;
    CHECK_FALSE(SelectableText::HasSelection(ctx));
}

TEST_CASE("SelectableText HasSelection rejects stale (out-of-range) endpoints") {
    Context ctx = MakeFixture();
    ctx.hasSelection = true;
    ctx.selStartSeg = 0;
    ctx.selStartChar = 0;
    ctx.selEndSeg = 99; // past end of segments vector
    ctx.selEndChar = 0;
    CHECK_FALSE(SelectableText::HasSelection(ctx));
}

TEST_CASE("SelectableText GetSelectedText slices mid-segment to mid-segment") {
    Context ctx = MakeFixture();
    // Select "llo wor" — from seg0 char 2 ("Hello"[2..]) through seg1 char 4 (" world"[..4]).
    ctx.hasSelection = true;
    ctx.selStartSeg = 0;
    ctx.selStartChar = 2;
    ctx.selEndSeg = 1;
    ctx.selEndChar = 4;
    CHECK(SelectableText::HasSelection(ctx));
    CHECK(SelectableText::GetSelectedText(ctx) == "llo wor");
}

TEST_CASE("SelectableText GetSelectedText normalises reverse selection") {
    Context ctx = MakeFixture();
    // End before start in doc-order — must produce the same forward slice.
    ctx.hasSelection = true;
    ctx.selStartSeg = 1;
    ctx.selStartChar = 4;
    ctx.selEndSeg = 0;
    ctx.selEndChar = 2;
    CHECK(SelectableText::HasSelection(ctx));
    CHECK(SelectableText::GetSelectedText(ctx) == "llo wor");
}

TEST_CASE("SelectableText GetSelectedText inserts newline at block boundary") {
    Context ctx = MakeFixture();
    // Cross from seg1 (block 0) into seg2 (block 1). Expect a newline injected
    // before the block-1 bytes.
    ctx.hasSelection = true;
    ctx.selStartSeg = 1;
    ctx.selStartChar = 1; // " world"[1..] -> "world"
    ctx.selEndSeg = 2;
    ctx.selEndChar = 4; // "Next line"[..4] -> "Next"
    const std::string sel = SelectableText::GetSelectedText(ctx);
    CHECK(sel == "world\nNext");
}

TEST_CASE("SelectableText GetSelectedText single-segment slice") {
    Context ctx = MakeFixture();
    ctx.hasSelection = true;
    ctx.selStartSeg = 0;
    ctx.selStartChar = 0;
    ctx.selEndSeg = 0;
    ctx.selEndChar = 5; // entire "Hello"
    CHECK(SelectableText::GetSelectedText(ctx) == "Hello");
}

TEST_CASE("SelectableText GetSelectedText empty when no selection flag") {
    Context ctx = MakeFixture();
    ctx.hasSelection = false;
    ctx.selStartSeg = 0;
    ctx.selStartChar = 0;
    ctx.selEndSeg = 2;
    ctx.selEndChar = 9;
    CHECK(SelectableText::GetSelectedText(ctx).empty());
}

TEST_CASE("SelectableText GetSelectedText clamps out-of-range char offsets") {
    Context ctx = MakeFixture();
    // selEndChar past end-of-segment must be clamped to the segment length.
    ctx.hasSelection = true;
    ctx.selStartSeg = 0;
    ctx.selStartChar = 0;
    ctx.selEndSeg = 0;
    ctx.selEndChar = 999;
    CHECK(SelectableText::GetSelectedText(ctx) == "Hello");
}

TEST_CASE("SelectableText GetHoveredHref reflects hoveredSeg state") {
    Context ctx = MakeFixture();
    // Set a fake href on seg1.
    int marker = 0;
    ctx.segments[1].href = &marker;
    ctx.hoveredSeg = 1;
    CHECK(SelectableText::GetHoveredHref(ctx) == &marker);
    ctx.hoveredSeg = -1;
    CHECK(SelectableText::GetHoveredHref(ctx) == nullptr);
}

// ---- detail:: pure-geometry / selection-math helpers --------------------
// These carry no ImGui-context dependency, so the fixtures construct
// positioned segments directly. Goldens captured from the real helper runs.

namespace {

using SelectableText::detail::BoundingBox;
using SelectableText::detail::ComputeBoundingBox;
using SelectableText::detail::ComputeSegmentSpan;
using SelectableText::detail::HitTestSegmentIndex;
using SelectableText::detail::PointInSegment;
using SelectableText::detail::SegmentSpan;
using SelectableText::detail::SelectAllSegments;
using SelectableText::detail::SelectionEndpointsInRange;

// Two positioned segments on two visual lines:
//   seg0 at (10,20), width 30, lineH 16  -> rect [10..40] x [20..36]
//   seg1 at (10,40), width 50, lineH 16  -> rect [10..60] x [40..56]
Context MakeGeometryFixture() {
    Context ctx;
    Segment a;
    a.pos = ImVec2(10.0f, 20.0f);
    a.totalWidth = 30.0f;
    a.lineH = 16.0f;
    a.textOwned = "abc";
    ctx.segments.push_back(a);
    Segment b;
    b.pos = ImVec2(10.0f, 40.0f);
    b.totalWidth = 50.0f;
    b.lineH = 16.0f;
    b.textOwned = "defgh";
    ctx.segments.push_back(b);
    return ctx;
}

} // namespace

TEST_CASE("detail::ComputeBoundingBox unions all segment rects") {
    Context ctx = MakeGeometryFixture();
    const BoundingBox bb = ComputeBoundingBox(ctx.segments);
    CHECK(bb.min.x == doctest::Approx(10.0f));
    CHECK(bb.min.y == doctest::Approx(20.0f));
    CHECK(bb.max.x == doctest::Approx(60.0f)); // widest = seg1 right edge
    CHECK(bb.max.y == doctest::Approx(56.0f)); // tallest = seg1 bottom edge
}

TEST_CASE("detail::ComputeBoundingBox single segment is that segment's rect") {
    Context ctx;
    Segment a;
    a.pos = ImVec2(5.0f, 7.0f);
    a.totalWidth = 12.0f;
    a.lineH = 9.0f;
    ctx.segments.push_back(a);
    const BoundingBox bb = ComputeBoundingBox(ctx.segments);
    CHECK(bb.min.x == doctest::Approx(5.0f));
    CHECK(bb.min.y == doctest::Approx(7.0f));
    CHECK(bb.max.x == doctest::Approx(17.0f));
    CHECK(bb.max.y == doctest::Approx(16.0f));
}

TEST_CASE("detail::PointInSegment inclusive edges, outside misses") {
    Context ctx = MakeGeometryFixture();
    const Segment& s = ctx.segments[0];           // rect [10..40] x [20..36]
    CHECK(PointInSegment(s, 25.0f, 28.0f));       // interior
    CHECK(PointInSegment(s, 10.0f, 20.0f));       // top-left corner (inclusive)
    CHECK(PointInSegment(s, 40.0f, 36.0f));       // bottom-right corner (inclusive)
    CHECK_FALSE(PointInSegment(s, 9.0f, 28.0f));  // left of rect
    CHECK_FALSE(PointInSegment(s, 25.0f, 37.0f)); // below rect
}

TEST_CASE("detail::HitTestSegmentIndex first-hit-wins and -1 on miss") {
    Context ctx = MakeGeometryFixture();
    CHECK(HitTestSegmentIndex(ctx.segments, 25.0f, 28.0f) == 0);    // inside seg0
    CHECK(HitTestSegmentIndex(ctx.segments, 30.0f, 48.0f) == 1);    // inside seg1
    CHECK(HitTestSegmentIndex(ctx.segments, 100.0f, 100.0f) == -1); // outside both
}

TEST_CASE("detail::ComputeSegmentSpan endpoints, middle, clamp, reverse") {
    // Selection (aSeg=0,aChar=2) .. (bSeg=2,bChar=4); current segLen = 6.
    // Middle segment (i=1) selects the whole segment [0,6].
    const SegmentSpan mid = ComputeSegmentSpan(1, 0, 2, 2, 4, 6);
    CHECK(mid.from == 0);
    CHECK(mid.to == 6);
    // Start segment (i=0) selects [aChar, segLen].
    const SegmentSpan start = ComputeSegmentSpan(0, 0, 2, 2, 4, 6);
    CHECK(start.from == 2);
    CHECK(start.to == 6);
    // End segment (i=2) selects [0, bChar].
    const SegmentSpan end = ComputeSegmentSpan(2, 0, 2, 2, 4, 6);
    CHECK(end.from == 0);
    CHECK(end.to == 4);
    // Single-segment reverse (from>to) ordered, and clamp to segLen.
    const SegmentSpan rev = ComputeSegmentSpan(0, 0, 5, 0, 2, 3);
    CHECK(rev.from == 2);
    CHECK(rev.to == 3); // 5 clamped down to segLen 3
}

TEST_CASE("detail::SelectionEndpointsInRange matches the overlay guard") {
    Context ctx = MakeFixture();                 // 3 segments
    CHECK_FALSE(SelectionEndpointsInRange(ctx)); // hasSelection false
    ctx.hasSelection = true;
    ctx.selStartSeg = 0;
    ctx.selEndSeg = 2;
    CHECK(SelectionEndpointsInRange(ctx));
    ctx.selEndSeg = 3; // past end
    CHECK_FALSE(SelectionEndpointsInRange(ctx));
}

TEST_CASE("detail::SelectAllSegments spans all bytes; no-op when empty") {
    Context ctx = MakeFixture(); // seg2 = "Next line" (9 bytes)
    SelectAllSegments(ctx, 3);
    CHECK(ctx.hasSelection);
    CHECK(ctx.selStartSeg == 0);
    CHECK(ctx.selStartChar == 0);
    CHECK(ctx.selEndSeg == 2);
    CHECK(ctx.selEndChar == 9);

    Context empty;
    SelectAllSegments(empty, 0);
    CHECK_FALSE(empty.hasSelection);
}
