#pragma once
#include <cstdint>
#include <string>

// UI layout-mode identifiers + mobile-shell sub-state. Kept in a lightweight
// header (no ImGui dependency, mirroring SmatchetThemeIds.h) so TrackerConfig and
// other config-layer code can carry the values without pulling the full ImGui
// surface. Unlike SmatchetThemeIds.h (converter-free), this header carries inline
// string converters so callers round-trip the values without reaching into
// ConfigManager — a deliberate local choice for the mode/page/density enums.

// Persisted UI-mode preference. Auto resolves to a concrete EffectiveUiMode per
// frame by viewport width (hysteresis); Desktop/Mobile pin the mode.
enum class UiMode : std::uint8_t { Desktop = 0, Mobile = 1, Auto = 2 };

// Per-frame resolved mode (Auto collapsed to a concrete value). Never persisted —
// computed at the top of SmatchetUI::Draw and held across frames for hysteresis.
enum class EffectiveUiMode : std::uint8_t { Desktop = 0, Mobile = 1 };

// Mobile shell page identity (bottom-nav selection + home page). The string ids
// match the default MobileNavPages list ("grid","views","log","settings","ai").
enum class MobilePage : std::uint8_t { Grid = 0, Views = 1, Log = 2, Settings = 3, Ai = 4 };

// Touch hit-target density preset → ScaleAllSizes / font multiplier.
enum class MobileTouchDensity : std::uint8_t { Compact = 0, Comfortable = 1 };

// --- Inline string converters (round-trip through ConfigManager JSON keys). ---

inline const char* uiModeToString(UiMode m) {
    switch (m) {
    case UiMode::Desktop:
        return "desktop";
    case UiMode::Mobile:
        return "mobile";
    case UiMode::Auto:
        return "auto";
    }
    return "auto";
}

inline UiMode uiModeFromString(const std::string& s) {
    if (s == "desktop") {
        return UiMode::Desktop;
    }
    if (s == "mobile") {
        return UiMode::Mobile;
    }
    return UiMode::Auto;
}

inline const char* mobilePageToString(MobilePage p) {
    switch (p) {
    case MobilePage::Grid:
        return "grid";
    case MobilePage::Views:
        return "views";
    case MobilePage::Log:
        return "log";
    case MobilePage::Settings:
        return "settings";
    case MobilePage::Ai:
        return "ai";
    }
    return "grid";
}

inline MobilePage mobilePageFromString(const std::string& s) {
    if (s == "views") {
        return MobilePage::Views;
    }
    if (s == "log") {
        return MobilePage::Log;
    }
    if (s == "settings") {
        return MobilePage::Settings;
    }
    if (s == "ai") {
        return MobilePage::Ai;
    }
    return MobilePage::Grid;
}

// Human-facing label for a mobile page id (bottom-nav button, drawer row,
// Preferences nav editor). Distinct from mobilePageToString, which yields the
// persisted id. Unknown ids fall back to the raw id.
inline const char* mobileNavPageLabel(const std::string& id) {
    if (id == "grid") {
        return "Tickets";
    }
    if (id == "views") {
        return "Views";
    }
    if (id == "log") {
        return "Log";
    }
    if (id == "settings") {
        return "Settings";
    }
    if (id == "ai") {
        return "AI";
    }
    return id.c_str();
}

inline const char* mobileTouchDensityToString(MobileTouchDensity d) {
    switch (d) {
    case MobileTouchDensity::Compact:
        return "compact";
    case MobileTouchDensity::Comfortable:
        return "comfortable";
    }
    return "comfortable";
}

inline MobileTouchDensity mobileTouchDensityFromString(const std::string& s) {
    if (s == "compact") {
        return MobileTouchDensity::Compact;
    }
    return MobileTouchDensity::Comfortable;
}

// Hit-target / font enlargement factor applied on a Mobile/density flip
// (ScaleAllSizes + font-reload multiplier). Comfortable is the touch-first
// default; Compact trades reach for density. Tunable.
inline float mobileTouchDensityScale(MobileTouchDensity d) {
    switch (d) {
    case MobileTouchDensity::Compact:
        return 1.3f;
    case MobileTouchDensity::Comfortable:
        return 1.6f;
    }
    return 1.6f;
}
