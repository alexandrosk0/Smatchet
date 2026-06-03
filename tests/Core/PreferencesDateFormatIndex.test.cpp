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
