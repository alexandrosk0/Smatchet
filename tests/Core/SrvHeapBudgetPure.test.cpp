#include <doctest/doctest.h>

#include "Persistence/SrvHeapBudgetPure.h"
#include "TunableLimits.h"

using smatchet::persistence::kDx12SrvHeapDescriptors;
using smatchet::persistence::kSrvHeapReservedDescriptors;
using smatchet::persistence::SrvHeapCoversTextureCache;

TEST_CASE("SrvHeapBudgetPure: the shipped heap covers the default icon cache with room to spare") {
    // Read from the limits table, so raising the default past the heap's budget fails here, not at every launch.
    const int defaultEntries = smatchet::limits::LimitsConfig().ImageTextureCacheMaxEntries;
    CHECK(SrvHeapCoversTextureCache(kDx12SrvHeapDescriptors, defaultEntries));
    // The old 64- and 128-descriptor heaps did not: every texture past ~60 aliased one overflow slot.
    CHECK_FALSE(SrvHeapCoversTextureCache(64, defaultEntries));
    CHECK_FALSE(SrvHeapCoversTextureCache(128, defaultEntries));
}

TEST_CASE("SrvHeapBudgetPure: boundary is capacity minus the reserved slots") {
    const int cap = kDx12SrvHeapDescriptors;
    CHECK(SrvHeapCoversTextureCache(cap, cap - kSrvHeapReservedDescriptors));
    CHECK_FALSE(SrvHeapCoversTextureCache(cap, cap - kSrvHeapReservedDescriptors + 1));
}

TEST_CASE("SrvHeapBudgetPure: degenerate inputs are refused, never wrapped") {
    CHECK_FALSE(SrvHeapCoversTextureCache(0, 0));
    CHECK_FALSE(SrvHeapCoversTextureCache(-5, 1));
    CHECK_FALSE(SrvHeapCoversTextureCache(kDx12SrvHeapDescriptors, -1));
    CHECK_FALSE(SrvHeapCoversTextureCache(kSrvHeapReservedDescriptors, 1));
    CHECK(SrvHeapCoversTextureCache(kSrvHeapReservedDescriptors + 1, 1));
}

TEST_CASE("SrvHeapBudgetPure: the 10000-entry tunable ceiling exceeds the shipped heap (init warns)") {
    CHECK_FALSE(SrvHeapCoversTextureCache(kDx12SrvHeapDescriptors, 10000));
}
