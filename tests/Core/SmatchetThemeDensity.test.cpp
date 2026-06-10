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
using SmatchetTheme::ShouldRescaleHostDensity;

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

// --- ShouldRescaleHostDensity: relative live-style re-assert (#1071 item 15) ---
//
// Gates ReassertHostDensityScale's ScaleAllSizes(newScale/oldScale) — the RELATIVE
// transform applied when the host re-asserts a density (e.g. Android CONFIG_CHANGED
// lands a new DPI bucket) onto a live style already carrying oldScale. Distinct from
// the absolute ShouldApplyDensityScale above: this one cancels the previously stored
// scale rather than comparing against identity.

TEST_CASE("ShouldRescaleHostDensity: real DPI-bucket change applies the relative factor") {
    // The headline case: 420dpi (2.62) -> 560dpi (3.50) on a fold/unfold. Both positive,
    // different, so multiply the live style by 3.50/2.62.
    CHECK(ShouldRescaleHostDensity(2.62f, 3.50f));
    // And the reverse (unfold -> fold) shrinks it.
    CHECK(ShouldRescaleHostDensity(3.50f, 2.62f));
}

TEST_CASE("ShouldRescaleHostDensity: no movement is a no-op") {
    // Re-assert at the same scale (CONFIG_CHANGED with no DPI delta) — factor would be
    // 1.0; skip the identity multiply.
    CHECK_FALSE(ShouldRescaleHostDensity(2.62f, 2.62f));
    CHECK_FALSE(ShouldRescaleHostDensity(1.0f, 1.0f));
}

TEST_CASE("ShouldRescaleHostDensity: non-positive old has no live factor to cancel") {
    // Without a prior positive scale baked into the style there is nothing to divide by;
    // dividing by 0 / a negative would be undefined / style-inverting. The first ever
    // assert (old defaulted, never positively applied) takes the no-op path here and the
    // absolute apply elsewhere.
    CHECK_FALSE(ShouldRescaleHostDensity(0.0f, 2.0f));
    CHECK_FALSE(ShouldRescaleHostDensity(-1.0f, 2.0f));
}

TEST_CASE("ShouldRescaleHostDensity: non-positive / NaN new is rejected (self-contained guard)") {
    // ReassertHostDensityScale's own !(newScale>0) guard returns before this in production,
    // but the predicate restates the guard so it is safe in isolation and NaN-safe.
    CHECK_FALSE(ShouldRescaleHostDensity(2.0f, 0.0f));
    CHECK_FALSE(ShouldRescaleHostDensity(2.0f, -1.0f));
    CHECK_FALSE(ShouldRescaleHostDensity(2.0f, std::nanf("")));
    CHECK_FALSE(ShouldRescaleHostDensity(std::nanf(""), 2.0f));
}
