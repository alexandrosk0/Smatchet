// Pure fallback-selection logic for the icon button helpers (Stream A of the
// jql-omnibox-search plan). The ImGui-rendering wrapper (SmatchetIconButton in
// SmatchetIconButtons.cpp) is NOT exercised here — it depends on
// SmatchetAreFaIconsLoaded() (SmatchetImGuiFonts.cpp), which the doctest rig does
// not link. We test the pure label-construction predicate that decides the
// icon-vs-text path, which is the actual correctness-bearing logic.

#include "Ui/SmatchetIconButtonLabel.h"

#include <doctest/doctest.h>

#include <string>

namespace {
// A representative Font Awesome UTF-8 glyph (ICON_FA_ARROWS_ROTATE, U+f021) — we
// use the literal so the test stays independent of the IconsFontAwesome6.h
// include path.
const char* const kIcon = "\xef\x80\xa1";
} // namespace

TEST_CASE("SmatchetIconButtonImGuiLabel: icon path keeps a stable unique id") {
    // Font loaded + a real glyph -> show the glyph, suffix the fallback text as a
    // hidden ImGui id so two same-glyph buttons on one window do not collide.
    const std::string label = SmatchetIconButtonImGuiLabel(true, kIcon, "Refresh View");
    CHECK(label == std::string(kIcon) + "##Refresh View");
    // The visible glyph is present and the human label is hidden behind "##".
    CHECK(label.find(kIcon) == 0u);
    CHECK(label.find("##Refresh View") != std::string::npos);
}

TEST_CASE("SmatchetIconButtonImGuiLabel: two same-glyph buttons get distinct ids") {
    const std::string a = SmatchetIconButtonImGuiLabel(true, kIcon, "Refresh View");
    const std::string b = SmatchetIconButtonImGuiLabel(true, kIcon, "Refresh");
    CHECK(a != b); // distinct ImGui ids despite the identical leading glyph
}

TEST_CASE("SmatchetIconButtonImGuiLabel: font absent -> bare text fallback") {
    const std::string label = SmatchetIconButtonImGuiLabel(false, kIcon, "Refresh View");
    CHECK(label == "Refresh View");
    CHECK(label.find(kIcon) == std::string::npos); // no glyph leaked into the text path
    CHECK(label.find("##") == std::string::npos);  // no hidden-id suffix in text mode
}

TEST_CASE("SmatchetIconButtonImGuiLabel: empty/null icon degrades to text even when loaded") {
    CHECK(SmatchetIconButtonImGuiLabel(true, "", "Refresh") == "Refresh");
    CHECK(SmatchetIconButtonImGuiLabel(true, nullptr, "Refresh") == "Refresh");
}

TEST_CASE("SmatchetIconButtonImGuiLabel: null fallback is treated as empty") {
    CHECK(SmatchetIconButtonImGuiLabel(false, kIcon, nullptr) == "");
    CHECK(SmatchetIconButtonImGuiLabel(true, kIcon, nullptr) == std::string(kIcon) + "##");
}

TEST_CASE("SmatchetIconLeadingLabel: icon-leading keeps the visible text label") {
    // Commit-style buttons (Save & Sync) keep their text and gain a leading glyph;
    // the visible text is the id, so no "##" suffix.
    const std::string label = SmatchetIconLeadingLabel(true, kIcon, "Save & Sync");
    CHECK(label == std::string(kIcon) + " Save & Sync");
    CHECK(label.find("##") == std::string::npos);
}

TEST_CASE("SmatchetIconLeadingLabel: font absent -> bare text label") {
    CHECK(SmatchetIconLeadingLabel(false, kIcon, "Save & Sync") == "Save & Sync");
}

TEST_CASE("SmatchetIconLeadingLabel: empty/null icon degrades to bare text") {
    CHECK(SmatchetIconLeadingLabel(true, "", "Apply & Sync") == "Apply & Sync");
    CHECK(SmatchetIconLeadingLabel(true, nullptr, "Apply & Sync") == "Apply & Sync");
    CHECK(SmatchetIconLeadingLabel(true, kIcon, nullptr) == std::string(kIcon) + " ");
}
