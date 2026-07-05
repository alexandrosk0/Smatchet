// SmatchetThemeDensity.test.cpp — mobile host-injection seam #13.
//
// Unit-tests the pure ShouldApplyDensityScale predicate that ReapplyHostDensityScale
// uses to decide whether to re-assert the host-injected UI density scale on a freshly
// rebuilt ImGui style. Pure / ImGui-free, so it runs on the Windows doctest rig (the
// rest of SmatchetTheme is ImGui-bound). Pins the desktop-inert identity (1.0 -> false),
// the caller-error guards (0 / negatives / NaN -> false), and the real-density path.

#include "SmatchetThemeDensity.h"

#include "doctest/doctest.h"

#include <cmath>  // std::nanf
#include <limits> // std::numeric_limits

using SmatchetTheme::ComposeFontDensityScale;
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

// --- ComposeFontDensityScale: fold the OS accessibility font scale onto the DPI density ---
//
// The mobile host reads two independent scales (AConfiguration density bucket + the JNI-read
// Configuration.fontScale) and multiplies them into the single scale used to size the font atlas.
// Pure / ImGui-free, so it pins on the doctest rig. Covers the identity, the composite multiply, the
// [kMinFontScale, kMaxFontScale] font clamp, and the non-finite / non-positive caller-error fallbacks
// for BOTH inputs. The host feeds this into the atlas px only — density stays raw for HostDensityScale.

TEST_CASE("ComposeFontDensityScale: identity (desktop default) is 1.0") {
    CHECK(ComposeFontDensityScale(1.0f, 1.0f) == doctest::Approx(1.0f));
}

TEST_CASE("ComposeFontDensityScale: the two scales multiply") {
    // A Large-font user (1.3) on a high-DPI phone (2.62) wants both bumps stacked.
    CHECK(ComposeFontDensityScale(2.62f, 1.3f) == doctest::Approx(2.62f * 1.3f));
    CHECK(ComposeFontDensityScale(2.0f, 1.5f) == doctest::Approx(3.0f));
}

TEST_CASE("ComposeFontDensityScale: font 1.0 passes the density through unchanged") {
    // No accessibility bump → the result is exactly the DPI density (the pre-fontScale behaviour).
    CHECK(ComposeFontDensityScale(2.62f, 1.0f) == doctest::Approx(2.62f));
    CHECK(ComposeFontDensityScale(0.75f, 1.0f) == doctest::Approx(0.75f));
}

TEST_CASE("ComposeFontDensityScale: fontScale is clamped to [kMinFontScale, kMaxFontScale]") {
    // Below "Small" (0.85) and above the Android-14 ceiling (2.0) are clamped so a pathological OEM
    // value can't blow the atlas rebuild past the Pillar-2 budget or exhaust texture memory.
    CHECK(ComposeFontDensityScale(1.0f, 0.5f) == doctest::Approx(SmatchetTheme::kMinFontScale));
    CHECK(ComposeFontDensityScale(1.0f, 3.0f) == doctest::Approx(SmatchetTheme::kMaxFontScale));
    // The bounds themselves are honoured exactly (no off-by-one clamp).
    CHECK(ComposeFontDensityScale(1.0f, SmatchetTheme::kMinFontScale) == doctest::Approx(SmatchetTheme::kMinFontScale));
    CHECK(ComposeFontDensityScale(1.0f, SmatchetTheme::kMaxFontScale) == doctest::Approx(SmatchetTheme::kMaxFontScale));
}

TEST_CASE("ComposeFontDensityScale: non-finite / non-positive fontScale falls back to no bump") {
    // A bogus / absent accessibility value must degrade to the raw density, never a collapsed UI.
    CHECK(ComposeFontDensityScale(2.0f, 0.0f) == doctest::Approx(2.0f));
    CHECK(ComposeFontDensityScale(2.0f, -1.0f) == doctest::Approx(2.0f));
    CHECK(ComposeFontDensityScale(2.0f, std::nanf("")) == doctest::Approx(2.0f));
    CHECK(ComposeFontDensityScale(2.0f, std::numeric_limits<float>::infinity()) == doctest::Approx(2.0f));
}

TEST_CASE("ComposeFontDensityScale: non-finite / non-positive density falls back to 1.0") {
    // Matches ResolveDensityScale's own default for the no-DPI AConfiguration buckets; the clamped
    // font scale then stands alone.
    CHECK(ComposeFontDensityScale(0.0f, 1.5f) == doctest::Approx(1.5f));
    CHECK(ComposeFontDensityScale(-2.0f, 1.5f) == doctest::Approx(1.5f));
    CHECK(ComposeFontDensityScale(std::nanf(""), 1.5f) == doctest::Approx(1.5f));
    CHECK(ComposeFontDensityScale(std::numeric_limits<float>::infinity(), 1.5f) == doctest::Approx(1.5f));
}
