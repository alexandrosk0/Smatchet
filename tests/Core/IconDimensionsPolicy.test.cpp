// Pure-logic coverage of IconDimensionsWithinCap — the pre-decode accept/reject gate that lets
// SmatchetImageTextureCache reject an oversized image from the cheap stbi_info header read
// BEFORE the full stb decode allocates pixel storage (memory-pressure DoS, SECURITY_AUDIT
// 2026-06-13 synthesis #9). These cases pin the side caps, the non-positive guards, and the
// overflow-safe pixel-budget math against the values the cache wires in (512 px, 1 MiB).

#include "Persistence/IconDimensionsPolicy.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <limits>

using smatchet::IconDimensionsWithinCap;

namespace {
constexpr int kCap = 512;
constexpr std::size_t kPixelBudget = static_cast<std::size_t>(512) * 512u * 4u; // 1 MiB
} // namespace

TEST_CASE("IconDimensionsWithinCap: a normal in-cap image is accepted") {
    CHECK(IconDimensionsWithinCap(64, 64, kCap, kPixelBudget));
    CHECK(IconDimensionsWithinCap(512, 512, kCap, kPixelBudget));
    CHECK(IconDimensionsWithinCap(1, 1, kCap, kPixelBudget));
}

TEST_CASE("IconDimensionsWithinCap: a side over the dimension cap is rejected") {
    CHECK_FALSE(IconDimensionsWithinCap(513, 64, kCap, kPixelBudget));
    CHECK_FALSE(IconDimensionsWithinCap(64, 513, kCap, kPixelBudget));
    CHECK_FALSE(IconDimensionsWithinCap(100000, 100000, kCap, kPixelBudget));
}

TEST_CASE("IconDimensionsWithinCap: non-positive dimensions are rejected") {
    CHECK_FALSE(IconDimensionsWithinCap(0, 64, kCap, kPixelBudget));
    CHECK_FALSE(IconDimensionsWithinCap(64, 0, kCap, kPixelBudget));
    CHECK_FALSE(IconDimensionsWithinCap(-1, -1, kCap, kPixelBudget));
    CHECK_FALSE(IconDimensionsWithinCap(64, 64, 0, kPixelBudget));
}

TEST_CASE("IconDimensionsWithinCap: pixel-budget guard rejects in-cap-but-over-budget") {
    // Both sides at the cap fit the side check; a budget smaller than 512x512x4 must still
    // reject — the aggregate-pixel guard catches a cap that grows faster than the budget.
    CHECK_FALSE(IconDimensionsWithinCap(512, 512, kCap, kPixelBudget - 1u));
    CHECK(IconDimensionsWithinCap(512, 512, kCap, kPixelBudget));
}

TEST_CASE("IconDimensionsWithinCap: huge declared dimensions cannot overflow the size_t product") {
    // The attacker-controlled header path: a giant declared size is rejected by the side cap
    // long before the byte multiply runs, so width*height*4 never wraps. Even with INT_MAX
    // sides and a max budget the side cap fires first.
    const int huge = std::numeric_limits<int>::max();
    CHECK_FALSE(IconDimensionsWithinCap(huge, huge, kCap, std::numeric_limits<std::size_t>::max()));
    CHECK_FALSE(IconDimensionsWithinCap(huge, 1, kCap, std::numeric_limits<std::size_t>::max()));
}
