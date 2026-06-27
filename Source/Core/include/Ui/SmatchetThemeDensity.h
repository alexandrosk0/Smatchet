#pragma once

#include <cmath> // std::isfinite

namespace SmatchetTheme {

/** Should the host-injected UI density scale be re-applied on top of a freshly
 *  rebuilt ImGui style?
 *
 *  Pure, ImGui-free predicate so it is unit-testable on the Windows doctest rig
 *  (the rest of SmatchetTheme is ImGui-bound). `ReapplyHostDensityScale` calls
 *  this to decide whether to invoke `ImGuiStyle::ScaleAllSizes`.
 *
 *  Returns true only for a meaningful positive scale that differs from 1.0:
 *   - `scale <= 0` (incl. caller-error 0 and negatives) → false: scaling by a
 *     non-positive factor would collapse/invert the style; leave it untouched.
 *   - `scale == 1.0` → false: identity, no work needed (desktop default — the
 *     seam stays inert and ScaleAllSizes is never called).
 *   - any other positive value (e.g. 0.5, 2.0) → true: a real density change.
 *
 *  NaN yields false (NaN compares false to both `> 0.0f` and `!= 1.0f` short-
 *  circuits irrelevant once the first clause is false), matching the
 *  "leave the style intact on caller error" contract.
 *
 *  Note this is a DIFFERENT predicate from `ApplyUiDensityScale`'s own
 *  `!(densityScale > 0.0f)` boot-time guard: that one accepts 1.0 (stores it so
 *  later rebuilds know the host explicitly chose identity) and only rejects
 *  non-positive/NaN. Do not unify the two — they answer different questions. */
inline bool ShouldApplyDensityScale(float scale) { return scale > 0.0f && scale != 1.0f; }

/** Should `ReassertHostDensityScale` transform the LIVE ImGui style — and, by
 *  implication, by the relative factor `newScale / oldScale`?
 *
 *  Pure, ImGui-free predicate so it is unit-testable on the Windows doctest rig.
 *  `ReassertHostDensityScale` calls this to decide whether to invoke
 *  `ImGuiStyle::ScaleAllSizes(newScale / oldScale)` after a host re-assert (e.g.
 *  Android `APP_CMD_CONFIG_CHANGED` lands a new DPI bucket on a style that
 *  already carries `oldScale`).
 *
 *  ScaleAllSizes is multiplicative and the live style already bakes in
 *  `oldScale`, so multiplying by `newScale / oldScale` lands it on `newScale`
 *  without re-stacking `oldScale`. Returns true only when that move is both
 *  well-defined and meaningful:
 *   - `newScale > 0`  — caller-error guard (0 / negative / NaN). The
 *     `ReassertHostDensityScale` `!(newScale > 0.0f)` guard returns before this
 *     is reached in production; restated here so the predicate is self-contained
 *     and NaN-safe (NaN compares false to `> 0.0f`).
 *   - `oldScale > 0`  — a prior positive scale exists to divide by; without one
 *     there is no live factor baked into the style to cancel (and `newScale /
 *     oldScale` would be undefined / inverting for a non-positive `oldScale`).
 *   - `oldScale != newScale` — otherwise the factor is 1.0; skip the identity
 *     multiply (first call after a matching boot-time apply, or no DPI movement).
 *
 *  Distinct from `ShouldApplyDensityScale`, which gates an ABSOLUTE re-apply onto
 *  a freshly-rebuilt style (vs 1.0); this gates a RELATIVE transform of the live
 *  style (vs the previously stored scale). Do not unify — different questions. */
inline bool ShouldRescaleHostDensity(float oldScale, float newScale) {
    return newScale > 0.0f && oldScale > 0.0f && oldScale != newScale;
}

/** Android accessibility "Font size" range we honour, clamping the OS-reported
 *  Configuration.fontScale into it before composing. 0.85 = the "Small" setting,
 *  2.0 = the Android-14 non-linear-font-scaling ceiling. Bounding the multiplier
 *  here keeps the composed atlas size finite: a pathological OEM value (some skins
 *  report 0, or > 3) can't blow the CONFIG_CHANGED font-atlas rebuild past the
 *  Pillar-2 100 ms UI-block budget or exhaust GPU texture memory. */
constexpr float kMinFontScale = 0.85f;
constexpr float kMaxFontScale = 2.0f;

/** Compose the OS accessibility font-scale multiplier onto the host display-density
 *  scale, producing the single UI scale the mobile host feeds to BOTH the density
 *  seam (ApplyUiDensityScale / ReassertHostDensityScale) and the font-atlas pixel
 *  size.
 *
 *  Pure, ImGui-free, header-only so it is unit-testable on the Windows doctest rig
 *  (the rest of SmatchetTheme is ImGui-bound). The Android host reads two
 *  independent scales:
 *   - `densityScale` — the display's physical DPI bucket (AConfiguration_getDensity
 *     / 160; ~1.0 mdpi, ~2.6 on a 420-dpi phone). Already the seam's input today.
 *   - `fontScale` — the user's system "Font size" accessibility preference
 *     (android.content.res.Configuration.fontScale; 1.0 default, 0.85 Small,
 *     1.15/1.30 Large/Largest, up to ~2.0 with the accessibility ramps). Read over
 *     JNI; the NDK AConfiguration exposes no font-scale getter.
 *
 *  The two MULTIPLY: a Large-font user on a high-DPI phone wants both the DPI
 *  enlargement and the accessibility bump applied to the text. The composed scale
 *  sizes the font-atlas pixel size ONLY; style metrics (padding / hit targets) stay
 *  on the raw DPI density via ApplyUiDensityScale, so HostDensityScale() (the Auto
 *  UI-mode logical-width divisor) is unaffected by the font scale. Growing metrics by
 *  the font scale too is a Scope-2 follow-up (a separate host-injection seam).
 *
 *  Guards (the host passes raw JNI / NDK values, so harden here):
 *   - A non-finite or non-positive `densityScale` falls back to 1.0 (matches
 *     ResolveDensityScale's own default for the no-DPI AConfiguration buckets).
 *   - A non-finite or non-positive `fontScale` is treated as 1.0 (absent / bogus
 *     accessibility value → no bump, never a collapsed UI).
 *   - `fontScale` is clamped to [kMinFontScale, kMaxFontScale] before the multiply.
 *     The density factor is trusted un-clamped (a bounded DPI bucket).
 *
 *  Returns densityScale * clamp(fontScale). Desktop never calls this (no
 *  Configuration.fontScale); the seam stays inert there. */
inline float ComposeFontDensityScale(float densityScale, float fontScale) {
    const float density = (std::isfinite(densityScale) && densityScale > 0.0f) ? densityScale : 1.0f;
    float font = (std::isfinite(fontScale) && fontScale > 0.0f) ? fontScale : 1.0f;
    if (font < kMinFontScale) {
        font = kMinFontScale;
    }
    if (font > kMaxFontScale) {
        font = kMaxFontScale;
    }
    return density * font;
}

} // namespace SmatchetTheme
