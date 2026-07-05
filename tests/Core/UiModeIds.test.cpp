#include <doctest/doctest.h>

#include "SmatchetLocalization.h"
#include "SmatchetUiModeIds.h"

#include <string>
#include <vector>

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
    // mobileNavPageLabel now localizes the known labels (mobile.page.* keys), so pin
    // the language explicitly — the English expectations below must not depend on
    // which test flipped the global language last.
    SmatchetLocalization::SetLanguage("en-US");
    CHECK(std::string(mobileNavPageLabel("grid")) == "Tickets");
    CHECK(std::string(mobileNavPageLabel("views")) == "Views");
    CHECK(std::string(mobileNavPageLabel("log")) == "Log");
    CHECK(std::string(mobileNavPageLabel("settings")) == "Settings");
    CHECK(std::string(mobileNavPageLabel("ai")) == "AI");
    // Unknown id → raw id passthrough (custom nav pages).
    CHECK(std::string(mobileNavPageLabel("custom")) == "custom");

    // fr-FR: known ids translate; unknown ids still pass through verbatim.
    SmatchetLocalization::SetLanguage("fr-FR");
    CHECK(std::string(mobileNavPageLabel("views")) == u8"Vues");
    CHECK(std::string(mobileNavPageLabel("custom")) == "custom");
    SmatchetLocalization::SetLanguage("en-US");
}

TEST_CASE("mobileNavPageLabel returns owning storage — labels survive past the call") {
    // #1118 lifetime contract: mobileNavPageLabel returns std::string by value, not
    // const char*. The Preferences home-page combo collects the labels into a vector
    // and reads them after the call; a const char* return would have handed back
    // id.c_str() for the echo branch — a pointer into the (now-destroyed) caller
    // argument. By value, the stored labels stay valid. This test mirrors that
    // collect-then-read pattern and would dangle (UB) under the old signature.
    SmatchetLocalization::SetLanguage("en-US");
    std::vector<std::string> nav;
    nav.push_back("grid");
    nav.push_back("views");
    nav.push_back("custom-board"); // exercises the echo branch — the dangling case

    std::vector<std::string> labels;
    for (const std::string& id : nav) {
        labels.push_back(mobileNavPageLabel(id));
    }
    // nav (the source of the echoed id) is still alive here, but the labels own
    // their own storage regardless — the echo branch copied, it did not alias.
    CHECK(labels.size() == 3);
    CHECK(labels[0] == "Tickets");
    CHECK(labels[1] == "Views");
    CHECK(labels[2] == "custom-board");
}

TEST_CASE("resolveAutoEffectiveUiMode — width thresholds + hysteresis dead band") {
    // Below the enter width -> Mobile regardless of the prior decision. A 420-dpi phone
    // (1080 physical px / 2.625 density ~= 411 logical px) lands here; the pre-fix code
    // compared the raw 1080 px and wrongly stayed Desktop.
    CHECK(resolveAutoEffectiveUiMode(411.0f, EffectiveUiMode::Desktop) == EffectiveUiMode::Mobile);
    CHECK(resolveAutoEffectiveUiMode(720.0f, EffectiveUiMode::Desktop) == EffectiveUiMode::Mobile);

    // At/above the exit width -> Desktop regardless of the prior decision.
    CHECK(resolveAutoEffectiveUiMode(860.0f, EffectiveUiMode::Mobile) == EffectiveUiMode::Desktop);
    CHECK(resolveAutoEffectiveUiMode(1920.0f, EffectiveUiMode::Mobile) == EffectiveUiMode::Desktop);

    // Dead band (720 < w < 860) holds the prior frame's decision so a near-breakpoint
    // resize never flaps.
    CHECK(resolveAutoEffectiveUiMode(800.0f, EffectiveUiMode::Mobile) == EffectiveUiMode::Mobile);
    CHECK(resolveAutoEffectiveUiMode(800.0f, EffectiveUiMode::Desktop) == EffectiveUiMode::Desktop);
}

TEST_CASE("resolveEffectiveUiMode — explicit pin > host desktop hint > width auto") {
    // Explicit Desktop/Mobile pins win regardless of the host hint or viewport width.
    CHECK(resolveEffectiveUiMode(UiMode::Desktop, false, 400.0f, EffectiveUiMode::Mobile) == EffectiveUiMode::Desktop);
    // The host "prefers desktop" hint does NOT override an explicit Mobile pin.
    CHECK(resolveEffectiveUiMode(UiMode::Mobile, true, 1920.0f, EffectiveUiMode::Desktop) == EffectiveUiMode::Mobile);

    // A6: on Auto, a host that prefers desktop (ChromeOS / ARC) resolves Desktop even at a
    // mobile-width viewport — the width auto is bypassed.
    CHECK(resolveEffectiveUiMode(UiMode::Auto, true, 400.0f, EffectiveUiMode::Mobile) == EffectiveUiMode::Desktop);

    // Auto WITHOUT the host hint falls through to the width hysteresis (phones / tablets / desktop).
    CHECK(resolveEffectiveUiMode(UiMode::Auto, false, 400.0f, EffectiveUiMode::Desktop) == EffectiveUiMode::Mobile);
    CHECK(resolveEffectiveUiMode(UiMode::Auto, false, 1920.0f, EffectiveUiMode::Mobile) == EffectiveUiMode::Desktop);
    // Dead band still holds the prior decision when the hint is off.
    CHECK(resolveEffectiveUiMode(UiMode::Auto, false, 800.0f, EffectiveUiMode::Mobile) == EffectiveUiMode::Mobile);
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
