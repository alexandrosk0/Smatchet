// PrefsAssistantLocalization tests — pin the 5 Assistant Save/Discard prefs keys.
//
// The savediscard-locales branch added 5 keys (en + fr) used by the Preferences
// Assistant tab's explicit Save / Discard flow. A key typo (e.g. a mismatch
// between the .cpp table key and the call site) silently degrades T(key, fallback)
// to the English fallback even under the French locale. These cases prove each key
// is wired correctly by asserting:
//   - under fr-FR, each key resolves to its committed FRENCH value (NOT the
//     English fallback passed to T());
//   - under en-US with a null fallback, each key resolves to its committed
//     English table value (a missing/typo'd key would return "").
// The fr-FR fallbacks below are deliberately sentinel strings, so an unresolved
// key would surface as the sentinel and fail loudly.

#include "SmatchetLocalization.h"

#include <doctest/doctest.h>

#include <string>

namespace {

// Restore the default UI language after a test that flips it, so global
// localization state doesn't leak into later test cases in the same process.
struct LanguageResetGuard {
    ~LanguageResetGuard() { SmatchetLocalization::SetLanguage("en-US"); }
};

const char* const kSentinel = "<<UNRESOLVED-FALLBACK>>";

} // namespace

TEST_CASE("Assistant Save/Discard keys resolve to French under fr-FR") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("fr-FR");

    SUBCASE("prefs.assistant.saved_title") {
        const std::string fr =
            SmatchetLocalization::T("prefs.assistant.saved_title", kSentinel);
        CHECK(fr == u8"Paramètres de l'assistant");
        CHECK(fr != kSentinel);
    }
    SUBCASE("prefs.assistant.saved_body") {
        const std::string fr =
            SmatchetLocalization::T("prefs.assistant.saved_body", kSentinel);
        CHECK(fr == u8"Enregistré sur le disque.");
        CHECK(fr != kSentinel);
    }
    SUBCASE("prefs.assistant.unsaved") {
        const std::string fr =
            SmatchetLocalization::T("prefs.assistant.unsaved", kSentinel);
        CHECK(fr == u8"Modifications non enregistrées");
        CHECK(fr != kSentinel);
    }
    SUBCASE("prefs.assistant.no_changes") {
        const std::string fr =
            SmatchetLocalization::T("prefs.assistant.no_changes", kSentinel);
        CHECK(fr == u8"Aucune modification non enregistrée");
        CHECK(fr != kSentinel);
    }
    SUBCASE("prefs.footer.assistant.short") {
        const std::string fr =
            SmatchetLocalization::T("prefs.footer.assistant.short", kSentinel);
        CHECK(fr ==
              u8"Les réglages de l'assistant utilisent un Enregistrer / Annuler explicite. "
              u8"Les modifications non enregistrées affichent un * sur l'onglet.");
        CHECK(fr != kSentinel);
    }
}

TEST_CASE("Assistant Save/Discard keys hold their English table value") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("en-US");

    // Contract: under en-US, T(key, fallback) returns the caller-supplied
    // fallback (TranslateEntryLocked falls through to it), and only drops to the
    // table's own English string when fallback is null. Passing nullptr therefore
    // pins the committed English value for each key — and a missing/typo'd key
    // would return "" instead, failing loudly.
    CHECK(std::string(SmatchetLocalization::T("prefs.assistant.saved_title", nullptr)) ==
          "Assistant settings");
    CHECK(std::string(SmatchetLocalization::T("prefs.assistant.saved_body", nullptr)) ==
          "Saved to disk.");
    CHECK(std::string(SmatchetLocalization::T("prefs.assistant.unsaved", nullptr)) ==
          "Unsaved changes");
    CHECK(std::string(SmatchetLocalization::T("prefs.assistant.no_changes", nullptr)) ==
          "No unsaved changes");
    CHECK(std::string(SmatchetLocalization::T("prefs.footer.assistant.short", nullptr)) ==
          "Assistant settings use explicit Save / Discard. Unsaved edits show an * on the tab.");
}
