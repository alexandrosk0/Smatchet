// AsyncLoadGatePure.test.cpp — bucket-A coverage for the async-load gating predicates
// extracted in Issue #1925 (Pillar 2: the three UI-thread file reads moved to workers).
//
// These predicates are the whole correctness surface of the single-slot async loads. The
// production sites that call them:
//   Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp  — StartLoadSelected / PollLoadResult
//   Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp — MaybeKickDimensionParse + post-back
// Each failure mode below is a real user-visible defect, not a synthetic edge:
//   * a kick that fires while one is in flight move-assigns a live std::async future,
//     which BLOCKS the render thread — the exact freeze this Issue removed;
//   * a kick that never fires leaves a stale body on screen forever;
//   * a stale result applied unconditionally shows file A's contents under file B's name.

#include "AsyncLoadGatePure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::async_load::ResultIsCurrent;
using smatchet::async_load::ShouldKickLoad;
using smatchet::async_load::ShouldKickOnce;

TEST_CASE("ShouldKickLoad kicks only when the selection has moved off the cached payload") {
    // Fresh selection, nothing cached — the load must start.
    CHECK(ShouldKickLoad(false, "docs/plans/active/a.md", ""));
    // Selection moved to a different doc — reload.
    CHECK(ShouldKickLoad(false, "docs/plans/active/b.md", "docs/plans/active/a.md"));
    // Already showing exactly this doc — no re-read, or the viewer would loop on every
    // frame re-reading the same file off disk.
    CHECK_FALSE(ShouldKickLoad(false, "docs/plans/active/a.md", "docs/plans/active/a.md"));
    // Nothing selected (index empty, or a rescan cleared the selection) — nothing to read.
    CHECK_FALSE(ShouldKickLoad(false, "", "docs/plans/active/a.md"));
    CHECK_FALSE(ShouldKickLoad(false, "", ""));
}

TEST_CASE("ShouldKickLoad never overlaps an in-flight read") {
    // The in-flight term dominates every other input: re-assigning the future slot while a
    // std::async task is live blocks the caller until that task completes.
    CHECK_FALSE(ShouldKickLoad(true, "docs/plans/active/b.md", "docs/plans/active/a.md"));
    CHECK_FALSE(ShouldKickLoad(true, "docs/plans/active/a.md", ""));
    CHECK_FALSE(ShouldKickLoad(true, "", ""));
}

TEST_CASE("ShouldKickLoad is case- and whitespace-exact on the key") {
    // The key is compared verbatim — no path normalisation happens here, so two spellings
    // of the same file are two different keys and both would be read. Pinned so a future
    // "helpful" normalisation in the callers is a deliberate change, not a silent one.
    CHECK(ShouldKickLoad(false, "Docs/Plans/A.md", "docs/plans/a.md"));
    CHECK(ShouldKickLoad(false, "a.md ", "a.md"));
}

TEST_CASE("ShouldKickOnce latches after a parse lands, success or failure") {
    // Nothing has run yet for this entry — parse it.
    CHECK(ShouldKickOnce(false, false, false, false));
    // Landed (the `done` latch) — never re-run, even though the entry still qualifies.
    // A failed parse latches too: retrying it every frame would hammer an unreadable file,
    // and `done` is what keeps the thumbnail decode suppressed after a parse failure.
    CHECK_FALSE(ShouldKickOnce(false, true, false, false));
    // Already running — one slot only.
    CHECK_FALSE(ShouldKickOnce(true, false, false, false));
    // An upstream error (e.g. the download itself failed) means there is no file to read.
    CHECK_FALSE(ShouldKickOnce(false, false, true, false));
    // No key yet — the download has not produced a local path.
    CHECK_FALSE(ShouldKickOnce(false, false, false, true));
}

TEST_CASE("ShouldKickOnce blocks whenever any single suppressing term is set") {
    // Exhaustive over the 16 input combinations: exactly one of them kicks.
    int kicks = 0;
    for (int mask = 0; mask < 16; ++mask) {
        const bool inFlight = (mask & 1) != 0;
        const bool done = (mask & 2) != 0;
        const bool hasError = (mask & 4) != 0;
        const bool keyEmpty = (mask & 8) != 0;
        if (ShouldKickOnce(inFlight, done, hasError, keyEmpty)) {
            ++kicks;
            CHECK(mask == 0);
        }
    }
    CHECK(kicks == 1);
}

TEST_CASE("ResultIsCurrent applies a landed result only against the live key") {
    CHECK(ResultIsCurrent("docs/plans/active/a.md", "docs/plans/active/a.md"));
    // Selection moved while the read was in flight — the body describes a file the UI is
    // no longer showing.
    CHECK_FALSE(ResultIsCurrent("docs/plans/active/a.md", "docs/plans/active/b.md"));
    // Selection cleared mid-read (index rescan) — nothing to apply the payload to.
    CHECK_FALSE(ResultIsCurrent("docs/plans/active/a.md", ""));
}

TEST_CASE("ResultIsCurrent treats an empty result key as never current") {
    // Both kick gates refuse an empty key, so an empty result key can only come from a
    // defaulted/corrupted payload. Matching it against an empty live key would apply that
    // payload to "nothing selected".
    CHECK_FALSE(ResultIsCurrent("", ""));
    CHECK_FALSE(ResultIsCurrent("", "docs/plans/active/a.md"));
}

TEST_CASE("kick and apply gates agree on the same key") {
    // Round-trip of the production sequence: kick for `target`, worker echoes `target`
    // back, nothing moved in between -> the result applies, and once `loadedKey` is the
    // applied key the gate stops re-kicking.
    const std::string target = "docs/plans/active/a.md";
    REQUIRE(ShouldKickLoad(false, target, ""));
    CHECK(ResultIsCurrent(target, target));
    CHECK_FALSE(ShouldKickLoad(false, target, target));

    // And the losing race: the selection moved to `next` while `target` was in flight.
    // The landed `target` result is discarded, and the gate then kicks for `next`.
    const std::string next = "docs/plans/active/b.md";
    CHECK_FALSE(ResultIsCurrent(target, next));
    CHECK(ShouldKickLoad(false, next, ""));
}
