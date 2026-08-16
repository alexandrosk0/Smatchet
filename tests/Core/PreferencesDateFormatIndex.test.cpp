#include <doctest/doctest.h>

#include "SmatchetPreferencesUi_detail.h"

#include <string>

// Bucket-A coverage for the pure (ImGui-free) date-format index<->option helpers
// lifted out of DrawLocalAndAppearancePreferencesTabs during its function-size
// decomposition. SmatchetPreferencesUi_detail.h defines them `inline`, so the
// doctest rig needs no extra .cpp link. Goldens captured from the real Combo
// dispatch the Appearance tab used inline before the extraction.

using SmatchetPreferencesUiDetail::DateFormatIndexToOption;
using SmatchetPreferencesUiDetail::DateFormatOptionToIndex;

TEST_CASE("DateFormatOptionToIndex — known options + unknown fallback") {
    CHECK(DateFormatOptionToIndex("compact") == 0);
    CHECK(DateFormatOptionToIndex("always_relative") == 1);
    CHECK(DateFormatOptionToIndex("absolute_iso") == 2);
    CHECK(DateFormatOptionToIndex("absolute_friendly") == 3);
    // Unknown / empty falls back to the compact index (0), matching the legacy
    // `currentDateFormatIdx = 0` default.
    CHECK(DateFormatOptionToIndex("") == 0);
    CHECK(DateFormatOptionToIndex("garbage") == 0);
}

TEST_CASE("DateFormatIndexToOption — index to option string, out-of-range fallback") {
    CHECK(DateFormatIndexToOption(0) == "compact");
    CHECK(DateFormatIndexToOption(1) == "always_relative");
    CHECK(DateFormatIndexToOption(2) == "absolute_iso");
    CHECK(DateFormatIndexToOption(3) == "absolute_friendly");
    // Out-of-range indices fall back to "compact".
    CHECK(DateFormatIndexToOption(-1) == "compact");
    CHECK(DateFormatIndexToOption(4) == "compact");
    CHECK(DateFormatIndexToOption(99) == "compact");
}

TEST_CASE("DateFormat index<->option round-trips for every valid index") {
    for (int i = 0; i <= 3; ++i) {
        CHECK(DateFormatOptionToIndex(DateFormatIndexToOption(i)) == i);
    }
}

// --- Keybinding-row match cache invalidation (issue #2060) -------------------------
// The Shortcuts section's "does the query hit any binding row" answer is cached and
// recomputed only on edges. Its dirty flag is armed solely by the Preferences keybindings
// editor, but the binding table is also mutated by the toolbar / command-palette
// quick-bind, which calls only MarkKeybindingsDirty(). Comparing that one monotonic
// revision each frame is what closes the gap.

using SmatchetPreferencesUiDetail::TakeKeybindMatchCacheRevisionEdge;

TEST_CASE("TakeKeybindMatchCacheRevisionEdge: a steady revision arms nothing") {
    unsigned int seen = 7u;
    bool dirty = false;
    CHECK_FALSE(TakeKeybindMatchCacheRevisionEdge(7u, seen, dirty));
    CHECK(seen == 7u);
    CHECK_FALSE(dirty);
}

TEST_CASE("TakeKeybindMatchCacheRevisionEdge: a bump arms exactly one rescan") {
    // Frame 1: a quick-bind bumped the revision — rescan next.
    unsigned int seen = 0u;
    bool dirty = false;
    CHECK(TakeKeybindMatchCacheRevisionEdge(1u, seen, dirty));
    CHECK(seen == 1u);
    CHECK(dirty);

    // The scan runs and clears the flag; the following frames must not re-arm it.
    dirty = false;
    CHECK_FALSE(TakeKeybindMatchCacheRevisionEdge(1u, seen, dirty));
    CHECK_FALSE(dirty);
    CHECK_FALSE(TakeKeybindMatchCacheRevisionEdge(1u, seen, dirty));
    CHECK_FALSE(dirty);
}

TEST_CASE("TakeKeybindMatchCacheRevisionEdge: an already-dirty flag is never cleared") {
    // The Preferences editor's own arming must survive a no-edge frame.
    unsigned int seen = 3u;
    bool dirty = true;
    CHECK_FALSE(TakeKeybindMatchCacheRevisionEdge(3u, seen, dirty));
    CHECK(dirty);
}

TEST_CASE("TakeKeybindMatchCacheRevisionEdge: several bumps between frames still arm one rescan") {
    // A frame that misses intermediate bumps (Preferences closed, then reopened) sees a
    // single edge — the scan is a full recompute, so one is all it needs.
    unsigned int seen = 2u;
    bool dirty = false;
    CHECK(TakeKeybindMatchCacheRevisionEdge(9u, seen, dirty));
    CHECK(seen == 9u);
    CHECK(dirty);
}
