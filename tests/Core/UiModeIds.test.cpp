#include <doctest/doctest.h>

#include "Ui/SmatchetUiModeIds.h"

#include <string>

// Pure inline string/scale converters for the UI layout-mode + mobile-shell
// enums introduced by the dual-ui-mode batch (UiMode / MobilePage /
// MobileTouchDensity). SmatchetUiModeIds.h is ImGui-free and header-only, so the
// rig needs no extra .cpp link. These goldens pin the persisted JSON ids, the
// human-facing nav labels, and the unknown-input fallbacks the ui.mode command
// and ConfigManager round-trip relies on.

TEST_CASE("uiMode <-> string round-trips for every enumerator") {
    CHECK(std::string(uiModeToString(UiMode::Desktop)) == "desktop");
    CHECK(std::string(uiModeToString(UiMode::Mobile)) == "mobile");
    CHECK(std::string(uiModeToString(UiMode::Auto)) == "auto");

    CHECK(uiModeFromString("desktop") == UiMode::Desktop);
    CHECK(uiModeFromString("mobile") == UiMode::Mobile);
    CHECK(uiModeFromString("auto") == UiMode::Auto);
}

TEST_CASE("uiModeFromString — unknown input falls back to Auto") {
    // The ui.mode command relies on this default to keep an unrecognised
    // persisted value safe; the command itself rejects unknown CLI input before
    // reaching here.
    CHECK(uiModeFromString("") == UiMode::Auto);
    CHECK(uiModeFromString("MOBILE") == UiMode::Auto); // case-sensitive
    CHECK(uiModeFromString("tablet") == UiMode::Auto);
}

TEST_CASE("mobilePage <-> string round-trips and falls back to Grid") {
    CHECK(std::string(mobilePageToString(MobilePage::Grid)) == "grid");
    CHECK(std::string(mobilePageToString(MobilePage::Views)) == "views");
    CHECK(std::string(mobilePageToString(MobilePage::Log)) == "log");
    CHECK(std::string(mobilePageToString(MobilePage::Settings)) == "settings");
    CHECK(std::string(mobilePageToString(MobilePage::Ai)) == "ai");

    CHECK(mobilePageFromString("views") == MobilePage::Views);
    CHECK(mobilePageFromString("log") == MobilePage::Log);
    CHECK(mobilePageFromString("settings") == MobilePage::Settings);
    CHECK(mobilePageFromString("ai") == MobilePage::Ai);
    CHECK(mobilePageFromString("grid") == MobilePage::Grid);
    CHECK(mobilePageFromString("nope") == MobilePage::Grid);
}

TEST_CASE("mobileNavPageLabel maps known ids and echoes unknown ids verbatim") {
    CHECK(std::string(mobileNavPageLabel("grid")) == "Tickets");
    CHECK(std::string(mobileNavPageLabel("views")) == "Views");
    CHECK(std::string(mobileNavPageLabel("log")) == "Log");
    CHECK(std::string(mobileNavPageLabel("settings")) == "Settings");
    CHECK(std::string(mobileNavPageLabel("ai")) == "AI");
    // Unknown id → raw id passthrough (custom nav pages).
    CHECK(std::string(mobileNavPageLabel("custom")) == "custom");
}

TEST_CASE("mobileTouchDensity <-> string + scale factors") {
    CHECK(std::string(mobileTouchDensityToString(MobileTouchDensity::Compact)) == "compact");
    CHECK(std::string(mobileTouchDensityToString(MobileTouchDensity::Comfortable)) == "comfortable");

    CHECK(mobileTouchDensityFromString("compact") == MobileTouchDensity::Compact);
    CHECK(mobileTouchDensityFromString("comfortable") == MobileTouchDensity::Comfortable);
    CHECK(mobileTouchDensityFromString("loose") == MobileTouchDensity::Comfortable); // unknown → default

    // Comfortable (touch-first default) enlarges more than Compact.
    CHECK(mobileTouchDensityScale(MobileTouchDensity::Compact) == doctest::Approx(1.3f));
    CHECK(mobileTouchDensityScale(MobileTouchDensity::Comfortable) == doctest::Approx(1.6f));
    CHECK(mobileTouchDensityScale(MobileTouchDensity::Comfortable) >
          mobileTouchDensityScale(MobileTouchDensity::Compact));
}
