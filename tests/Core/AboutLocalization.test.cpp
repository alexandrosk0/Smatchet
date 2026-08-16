// AboutLocalization tests — pin the About-modal strings added by the
// selectable-values / right-click-link-menu change.
//
// Every chrome string in SmatchetAboutUi.cpp reaches the table through implicit
// source-string keying (`#define ImGui SmatchetLocalizedImGui` -> LabelFromSource
// -> TranslateSource), not through an explicit T(key, fallback) call. So the
// contract that actually matters is: the exact English literal in the .cpp is a
// TranslateSource key that resolves to French under fr-FR. Retyping a button
// label without updating the table would silently drop that string back to
// English for a French user, and nothing else in the build would notice.
//
// The right-click menu items ("Open in Browser" / "Copy Link") are the two
// strings with no other call site in the repo, so they have no incidental
// coverage at all. The hint line is the discoverability affordance for the whole
// feature -- if it stops translating, a French user is told nothing.
//
// "Check for Updates" is asserted absent-from-About in the other direction: the
// About modal deliberately no longer owns that action (Help does), and the
// about.check_updates row went with the button. The Help-menu and Preferences
// strings are distinct literals, so they must still resolve.

#include "SmatchetLocalization.h"

#include <doctest/doctest.h>

#include <string>

namespace {

// Restore the default UI language after a test that flips it, so global
// localization state doesn't leak into later test cases in the same process.
struct LanguageResetGuard {
    ~LanguageResetGuard() { SmatchetLocalization::SetLanguage("en-US"); }
};

} // namespace

TEST_CASE("About chrome strings translate through the source-string path") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("fr-FR");

    // TranslateSource returns its argument unchanged when the English literal is
    // not a table key, so `translated != source` is the real assertion here.
    SUBCASE("Open on GitHub") {
        const std::string fr = SmatchetLocalization::TranslateSource("Open on GitHub");
        CHECK(fr == u8"Ouvrir sur GitHub");
        CHECK(fr != "Open on GitHub");
    }
    SUBCASE("Open in Browser") {
        const std::string fr = SmatchetLocalization::TranslateSource("Open in Browser");
        CHECK(fr == u8"Ouvrir dans le navigateur");
        CHECK(fr != "Open in Browser");
    }
    SUBCASE("Copy Link") {
        const std::string fr = SmatchetLocalization::TranslateSource("Copy Link");
        CHECK(fr == u8"Copier le lien");
        CHECK(fr != "Copy Link");
    }
    SUBCASE("selection hint") {
        const char* const kHint = "Select any value to copy it. Right-click a link for actions.";
        const std::string fr = SmatchetLocalization::TranslateSource(kHint);
        CHECK(fr == u8"Sélectionnez une valeur pour la copier. "
                    u8"Clic droit sur un lien pour les actions.");
        CHECK(fr != kHint);
    }
}

TEST_CASE("About chrome strings hold their English table value") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("en-US");

    // Under en-US the source-string path is an identity, so this pins the literal
    // the .cpp passes rather than the table's own column. Combined with the fr-FR
    // case above, a typo on either side fails one of the two.
    CHECK(std::string(SmatchetLocalization::TranslateSource("Open on GitHub")) ==
          "Open on GitHub");
    CHECK(std::string(SmatchetLocalization::T("about.link_open", nullptr)) == "Open in Browser");
    CHECK(std::string(SmatchetLocalization::T("about.link_copy", nullptr)) == "Copy Link");
    CHECK(std::string(SmatchetLocalization::T("about.hint", nullptr)) ==
          "Select any value to copy it. Right-click a link for actions.");
}

TEST_CASE("The retired about.check_updates key is gone, the surviving ones are not") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("en-US");

    // T(key, nullptr) returns "" for an absent key. The About modal dropped its
    // duplicate update button, so this row must not come back.
    CHECK(std::string(SmatchetLocalization::T("about.check_updates", nullptr)).empty());

    // Help and Preferences still own the action under their own distinct literals.
    CHECK(std::string(SmatchetLocalization::TranslateSource("Check for Updates...")) ==
          "Check for Updates...");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Check for Updates Now")) ==
          "Check for Updates Now");

    // The section headers the modal still draws.
    CHECK(std::string(SmatchetLocalization::T("about.section.build", nullptr)) == "Build");
    CHECK(std::string(SmatchetLocalization::T("about.section.runtime", nullptr)) == "Runtime");
}
