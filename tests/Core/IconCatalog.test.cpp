// Pure-logic tests for the generated Font-Awesome 6 icon catalog and the
// name->glyph lookup (SmatchetIconCatalog.cpp). Guards the gen-fa-catalog.py
// output wiring (full FA6 set, not the old curated ~50) and the resolver
// contract. docs/plans/shipped/customizable-icon-toolbar.md § Verification (Bucket A).

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "SmatchetIconPickerUi.h"

TEST_CASE("Generated catalog holds the full FA6 set with every row populated") {
    const std::vector<SmatchetToolbarIconEntry>& cat = SmatchetToolbarIconCatalog();
    CHECK(cat.size() > 1000);  // full FA6 catalog, not the curated ~50
    for (const SmatchetToolbarIconEntry& e : cat) {
        REQUIRE(e.Name != nullptr);
        REQUIRE(e.Glyph != nullptr);
        CHECK(e.Name[0] != '\0');
        CHECK(e.Glyph[0] != '\0');
    }
}

TEST_CASE("SmatchetToolbarIconGlyph resolves curated-era and new names") {
    CHECK_FALSE(SmatchetToolbarIconGlyph("magnifying-glass").empty());  // was in the old set
    CHECK_FALSE(SmatchetToolbarIconGlyph("yin-yang").empty());          // absent from old set
    CHECK_FALSE(SmatchetToolbarIconGlyph("code-pull-request").empty()); // absent from old set
}

TEST_CASE("SmatchetToolbarIconGlyph returns empty for empty or unknown names") {
    CHECK(SmatchetToolbarIconGlyph("").empty());
    CHECK(SmatchetToolbarIconGlyph("definitely-not-a-real-icon-zzz").empty());
}

TEST_CASE("Lookup result matches the catalog entry's glyph") {
    const std::vector<SmatchetToolbarIconEntry>& cat = SmatchetToolbarIconCatalog();
    const std::string g = SmatchetToolbarIconGlyph(cat.front().Name);
    CHECK(g == std::string(cat.front().Glyph));
}
