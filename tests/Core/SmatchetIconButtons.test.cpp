// Pure fallback-selection logic for the icon button helpers (Stream A of the
// jql-omnibox-search plan). The ImGui-rendering wrapper (SmatchetIconButton in
// SmatchetIconButtons.cpp) is NOT exercised here — it depends on
// SmatchetAreFaIconsLoaded() (SmatchetImGuiFonts.cpp), which the doctest rig does
// not link. We test the pure label-construction predicate that decides the
// icon-vs-text path, which is the actual correctness-bearing logic.

#include "Ui/SmatchetIconButtonLabel.h"
#include "Ui/SubmitKeysPure.h"

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

// --- Key-hint button layout and the in-box placeholder (Ui/SubmitKeysPure.h) ----------------------
// The widgets in Ui/SmatchetSubmitKeys.h measure text and draw; these rules decide where the key name
// goes and when the placeholder shows.

TEST_CASE("KeyHintButtonLayout: the button grows to fit label, gap and key name") {
    using smatchet::submitkeys::ComputeKeyHintButtonLayout;
    // pad 8 + label 60 + gap 16 + hint 50 + pad 8 = 142
    const auto layout = ComputeKeyHintButtonLayout(60.0f, 50.0f, 16.0f, 8.0f, 0.0f);
    CHECK(layout.Width == doctest::Approx(142.0f));
    // The key name ends at the right padding.
    CHECK(layout.HintX == doctest::Approx(142.0f - 8.0f - 50.0f));
    // ...which leaves at least the gap after the label.
    CHECK(layout.HintX - (8.0f + 60.0f) == doctest::Approx(16.0f));
}

TEST_CASE("KeyHintButtonLayout: a caller's minimum width is kept, the key name stays at the right edge") {
    using smatchet::submitkeys::ComputeKeyHintButtonLayout;
    const auto wide = ComputeKeyHintButtonLayout(40.0f, 30.0f, 10.0f, 5.0f, 200.0f);
    CHECK(wide.Width == doctest::Approx(200.0f));
    CHECK(wide.HintX == doctest::Approx(200.0f - 5.0f - 30.0f));
    // A minimum narrower than the content does not shrink the button.
    const auto narrow = ComputeKeyHintButtonLayout(40.0f, 30.0f, 10.0f, 5.0f, 20.0f);
    CHECK(narrow.Width == doctest::Approx(90.0f));
}

TEST_CASE("KeyHintButtonLayout: no key name is a plain button, and negatives count as zero") {
    using smatchet::submitkeys::ComputeKeyHintButtonLayout;
    const auto plain = ComputeKeyHintButtonLayout(60.0f, 0.0f, 16.0f, 8.0f, 0.0f);
    CHECK(plain.Width == doctest::Approx(76.0f)); // no gap without a key name
    CHECK(plain.HintX == doctest::Approx(0.0f));
    const auto negative = ComputeKeyHintButtonLayout(-5.0f, 20.0f, -3.0f, -1.0f, 0.0f);
    CHECK(negative.Width == doctest::Approx(20.0f));
    CHECK(negative.HintX == doctest::Approx(0.0f));
}

TEST_CASE("ShowsPlaceholder: only an empty box shows it") {
    using smatchet::submitkeys::ShowsPlaceholder;
    CHECK(ShowsPlaceholder(""));
    CHECK(ShowsPlaceholder(nullptr));
    CHECK_FALSE(ShowsPlaceholder(" ")); // typed spaces are typed text
    CHECK_FALSE(ShowsPlaceholder("\n"));
    CHECK_FALSE(ShowsPlaceholder("note"));
}
