#pragma once

#include <string>

// Pure label-construction logic for the icon/text button helpers in
// SmatchetIconButtons.h, split into this ImGui-free, font-free header so the
// doctest rig (SmatchetTests — which does not link SmatchetImGuiFonts.cpp, the
// home of SmatchetAreFaIconsLoaded()) can exercise the fallback-selection
// decision as a pure predicate. See docs/plans jql-omnibox-search § Stream A.

// Icon-ONLY button label. When the Font Awesome icon font is loaded AND `icon`
// is a non-empty glyph, returns "<icon>##<fallbackLabel>": the glyph is shown
// while the "##<fallbackLabel>" suffix keeps the ImGui id stable + unique (two
// same-glyph refresh buttons on one window must not collide on a bare glyph id).
// When the font is absent (or `icon` is empty), returns the bare `fallbackLabel`
// text button. Null pointers are treated as empty strings.
inline std::string SmatchetIconButtonImGuiLabel(bool iconsLoaded, const char* icon, const char* fallbackLabel) {
    const std::string fallback = fallbackLabel ? fallbackLabel : "";
    const std::string glyph = icon ? icon : "";
    if (iconsLoaded && !glyph.empty()) {
        return glyph + "##" + fallback;
    }
    return fallback;
}

// Icon-LEADING button label for commit-style actions (Save & Sync / Apply &
// Sync) that KEEP their text label but gain a leading glyph. Returns
// "<icon> <text>" when the font is loaded (and the glyph is non-empty), else the
// bare `text`. The visible text doubles as the ImGui id, so no "##" id suffix is
// needed. Null pointers are treated as empty strings.
inline std::string SmatchetIconLeadingLabel(bool iconsLoaded, const char* icon, const char* text) {
    const std::string label = text ? text : "";
    const std::string glyph = icon ? icon : "";
    if (iconsLoaded && !glyph.empty()) {
        return glyph + " " + label;
    }
    return label;
}
