// SelectableTextRun bucket-A doctest — pure-logic coverage of doc-order range
// walk + char-offset slicing + reverse-selection normalisation +
// block-boundary newline insertion.
//
// The hit-test / overlay paths depend on ImFont + an ImGui context; those live
// under bucket E (deferred). Here we construct a synthetic Context directly
// (the struct + Segment are exposed in the public header so this is clean).

#include "SelectableTextRun.h"

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
