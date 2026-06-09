// SmatchetThemeDensity.test.cpp — mobile host-injection seam #13 (PR #1043).
//
// Unit-tests the pure ShouldApplyDensityScale predicate that ReapplyHostDensityScale
// uses to decide whether to re-assert the host-injected UI density scale on a freshly
// rebuilt ImGui style. Pure / ImGui-free, so it runs on the Windows doctest rig (the
// rest of SmatchetTheme is ImGui-bound). Pins the desktop-inert identity (1.0 -> false),
// the caller-error guards (0 / negatives / NaN -> false), and the real-density path.

#include "SmatchetThemeDensity.h"

#include "doctest/doctest.h"

#include <cmath> // std::nanf

using SmatchetTheme::ShouldApplyDensityScale;

TEST_CASE("ShouldApplyDensityScale: identity 1.0 is a no-op (desktop default)") {
    // Desktop never calls ApplyUiDensityScale, so g_hostDensityScale stays 1.0 and the
    // seam must stay inert — ScaleAllSizes is never invoked.
    CHECK_FALSE(ShouldApplyDensityScale(1.0f));
}

TEST_CASE("ShouldApplyDensityScale: non-positive scales are rejected") {
    // Scaling by 0 collapses every metric; a negative factor inverts the style. Both are
    // caller errors — leave the style untouched.
    CHECK_FALSE(ShouldApplyDensityScale(0.0f));
    CHECK_FALSE(ShouldApplyDensityScale(-1.0f));
    CHECK_FALSE(ShouldApplyDensityScale(-0.5f));
}

TEST_CASE("ShouldApplyDensityScale: NaN is rejected (matches !(x>0) intent)") {
    // NaN compares false to `> 0.0f`, so the predicate short-circuits to false —
    // matching the "leave the style intact on caller error" contract.
    CHECK_FALSE(ShouldApplyDensityScale(std::nanf("")));
}

TEST_CASE("ShouldApplyDensityScale: real positive density changes apply") {
    // A high-DPI touch host injects e.g. 2.0 (xxhdpi) or 0.5; both are meaningful,
    // differ from identity, and must scale.
    CHECK(ShouldApplyDensityScale(2.0f));
    CHECK(ShouldApplyDensityScale(0.5f));
    CHECK(ShouldApplyDensityScale(1.5f));
    // A value just off identity still applies (no epsilon dead-band by design).
    CHECK(ShouldApplyDensityScale(1.0001f));
}
