#include <doctest/doctest.h>

#include "Ui/SmatchetDockNodeIds.h"

#include <cstring>

using namespace SmatchetDockNodeIds;

// Pure-logic coverage for the default dock-slot table that drives both initial docking and the
// live layout-reset force-redock. Regression guard for the reset-layout float bug (reset-layout left windows
// floating because the iteration accessors below were the seam the reset relied on).

TEST_CASE("DefaultDockSlotForLayoutKey maps known keys to their dock slot") {
    CHECK(DefaultDockSlotForLayoutKey("active") == kCentralNode);
    CHECK(DefaultDockSlotForLayoutKey("views") == kViewsColumn);

    // Every other registered window defaults to the bottom panel.
    const char* bottomKeys[] = {"preferences", "log",   "performance", "scripting", "mcp",      "bulk_import",
                                "bulk_export", "audit", "annotate",    "plandocs",  "user_info"};
    for (const char* key : bottomKeys) {
        CHECK(DefaultDockSlotForLayoutKey(key) == kBottomPanel);
    }
}

TEST_CASE("DefaultDockSlotForLayoutKey returns 0 for overlays / unknown / degenerate input") {
    CHECK(DefaultDockSlotForLayoutKey(nullptr) == 0u);
    CHECK(DefaultDockSlotForLayoutKey("") == 0u);
    CHECK(DefaultDockSlotForLayoutKey("does_not_exist") == 0u);
    CHECK(DefaultDockSlotForLayoutKey("ACTIVE") == 0u); // case-sensitive
}

TEST_CASE("DefaultDockLayoutKey iteration accessors are consistent with the slot lookup") {
    const size_t count = DefaultDockLayoutKeyCount();
    CHECK(count == 13u);

    // In-range entries are non-empty and every one resolves to a real (non-zero) dock slot,
    // so the live-reset force-redock can never iterate onto a key the slot lookup rejects.
    for (size_t i = 0; i < count; ++i) {
        const char* key = DefaultDockLayoutKeyAt(i);
        REQUIRE(key != nullptr);
        CHECK(std::strlen(key) > 0);
        CHECK(DefaultDockSlotForLayoutKey(key) != 0u);
    }

    // Out-of-range index is the empty string, not UB.
    CHECK(std::strcmp(DefaultDockLayoutKeyAt(count), "") == 0);
    CHECK(std::strcmp(DefaultDockLayoutKeyAt(count + 100), "") == 0);
}
