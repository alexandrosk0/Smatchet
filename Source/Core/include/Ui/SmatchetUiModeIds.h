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

// Auto-mode width hysteresis. Enter Mobile at or below the max-enter width; exit to Desktop at or
// above the min-exit width; hold the previous decision inside the dead band so a near-breakpoint
// resize never flaps. Compared against a DPI-independent *logical* width (physical px / density),
// so a high-DPI phone with a large pixel count still reads as narrow.
constexpr float kMobileEnterMaxWidthPx = 720.0f;
constexpr float kMobileExitMinWidthPx = 860.0f;

// Resolve the Auto UI-mode to a concrete EffectiveUiMode from a logical viewport width, carrying the
// previous frame's decision through the dead band (hysteresis). Pure — the caller computes the
// logical width (raw io.DisplaySize.x divided by the host density scale) and passes the prior mode.
inline EffectiveUiMode resolveAutoEffectiveUiMode(float logicalWidthPx, EffectiveUiMode prev) {
    if (logicalWidthPx <= kMobileEnterMaxWidthPx) {
        return EffectiveUiMode::Mobile;
    }
    if (logicalWidthPx >= kMobileExitMinWidthPx) {
        return EffectiveUiMode::Desktop;
    }
    return prev; // dead band (720 < w < 860) — hold the prior frame's decision.
}

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
// persisted id. Returns by value (std::string) rather than const char*: the
// fallback echoes the raw `id` for an unrecognised (custom) page, and a
// const char* return would force that branch to hand back `id.c_str()` — a
// pointer into the caller's argument that dangles the moment the caller stores
// it past the call (the home-page combo collects labels into a vector). By
// value, every branch owns its storage. The known ids are exhaustive vs
// mobilePageToString and SanitizeMobileNav drops unknowns, so the echo branch
// is unreachable for sanitized input today, but it stays correct (and safe) if
// a future custom-page id ever reaches here.
inline std::string mobileNavPageLabel(const std::string& id) {
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
    return id;
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
