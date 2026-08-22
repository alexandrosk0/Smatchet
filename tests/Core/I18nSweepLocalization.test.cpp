// I18nSweepLocalization tests — pin a representative sample of the keys added by
// the user-facing-text-i18n-sweep (toast titles, command palette, toolbar editor,
// mobile shell, audit filters).
//
// Same failure mode as PrefsAssistantLocalization.test.cpp: a typo between the
// kEntries key and the call site silently degrades T(key, fallback) to the English
// fallback under fr-FR. Each case asserts the key resolves to its committed FRENCH
// value against a sentinel fallback, plus one source-map round-trip for the popup
// title paths that translate by English source (WindowTitleFromSource).

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

TEST_CASE("i18n-sweep keys resolve to French under fr-FR") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("fr-FR");

    SUBCASE("toast titles") {
        CHECK(std::string(SmatchetLocalization::T("toast.updates", kSentinel)) == u8"Mises à jour");
        CHECK(std::string(SmatchetLocalization::T("toast.view_saved", kSentinel)) == u8"Vue enregistrée");
        CHECK(std::string(SmatchetLocalization::T("toast.search", kSentinel)) == u8"Recherche");
        CHECK(std::string(SmatchetLocalization::T("toast.storage", kSentinel)) == u8"Stockage");
    }

    SUBCASE("command palette") {
        CHECK(std::string(SmatchetLocalization::T("cmdpalette.run", kSentinel)) == u8"Exécuter");
        CHECK(std::string(SmatchetLocalization::T("cmdpalette.hint", kSentinel)) ==
              u8"Saisissez une commande ou recherchez...");
    }

    SUBCASE("toolbar editor") {
        CHECK(std::string(SmatchetLocalization::T("toolbar.editor.title", kSentinel)) ==
              u8"Personnaliser la barre d'outils");
        CHECK(std::string(SmatchetLocalization::T("toolbar.kind.separator", kSentinel)) == u8"Séparateur");
    }

    SUBCASE("mobile shell") {
        CHECK(std::string(SmatchetLocalization::T("mobile.page.settings", kSentinel)) == u8"Paramètres");
        CHECK(std::string(SmatchetLocalization::T("mobile.grid.no_tickets", kSentinel)) == u8"Aucun ticket chargé.");
    }

    SUBCASE("audit filters") {
        // New Action-filter key; the Result items reuse the pre-existing audit.* rows.
        CHECK(std::string(SmatchetLocalization::T("audit.offline", kSentinel)) == u8"Hors ligne");
    }

    SUBCASE("bottom-panel drag gestures") {
        CHECK(std::string(SmatchetLocalization::T("panel.release_to_hide", kSentinel)) ==
              u8"Relâchez pour masquer le panneau");
        CHECK(std::string(SmatchetLocalization::T("panel.drag_to_open", kSentinel)) ==
              u8"Faites glisser vers le haut pour ouvrir le panneau");
    }

    SUBCASE("format keys keep their conversion specifiers") {
        // Format() falls back to the English literal when the translation's specifier
        // sequence diverges — a translated string that loses "%s" would silently revert
        // to English at every call site.
        CHECK(std::string(SmatchetLocalization::Format("userinfo.query_set", "View query set to %s.", "KEY-1")) ==
              u8"Requête de la vue définie sur KEY-1.");
        CHECK(std::string(SmatchetLocalization::Format("bulk.import_line_error", "Line %d: %s", 7, "bad row")) ==
              u8"Ligne 7 : bad row");
    }
}

TEST_CASE("i18n-sweep popup titles round-trip through the source map") {
    LanguageResetGuard restore;

    // en-US: sources with entries still return themselves unchanged.
    SmatchetLocalization::SetLanguage("en-US");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Update Available")) == "Update Available");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Pick Icon")) == "Pick Icon");

    // fr-FR: the wrapper's WindowTitleFromSource translates the visible part by English
    // source; these are the strings whose OpenPopup/Begin pairing relies on "###" IDs.
    SmatchetLocalization::SetLanguage("fr-FR");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Update Available")) == u8"Mise à jour disponible");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Pick Icon")) == u8"Choisir une icône");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Details")) == u8"Détails");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Notifications")) == u8"Notifications");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Select all")) == u8"Tout sélectionner");
}

TEST_CASE("i18n-sweep quick-create popup keys resolve to French under fr-FR") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("fr-FR");

    SUBCASE("popup body + toasts") {
        CHECK(std::string(SmatchetLocalization::T("toast.quick_create", kSentinel)) == u8"Création rapide");
        CHECK(std::string(SmatchetLocalization::T("quickcreate.summary", kSentinel)) == u8"Résumé");
        CHECK(std::string(SmatchetLocalization::T("quickcreate.refresh_context", kSentinel)) ==
              u8"Actualiser le contexte");
        CHECK(std::string(SmatchetLocalization::T("quickcreate.queued_offline", kSentinel)) ==
              u8"Hors ligne détecté ; mis en file d'attente.");
    }

    SUBCASE("preferences tab") {
        CHECK(std::string(SmatchetLocalization::T("quickcreate.prefs.engine_version", kSentinel)) ==
              u8"Version du moteur");
        CHECK(std::string(SmatchetLocalization::T("quickcreate.prefs.log_lines", kSentinel)) == u8"Lignes de log");
    }

    SUBCASE("window title translates by English source (### pairing)") {
        CHECK(std::string(SmatchetLocalization::TranslateSource("Quick Create Issue")) ==
              u8"Création rapide de ticket");
    }
}

TEST_CASE("preferences nav rail categories translate by English source under fr-FR") {
    LanguageResetGuard restore;
    SmatchetLocalization::SetLanguage("fr-FR");

    // DrawPrefsNav renders each category label through the localized-ImGui wrappers,
    // which resolve by English reverse lookup — the TitleEn string IS the key. Pin
    // every rail entry so a deleted/renamed row can't silently drop a category back
    // to English (the IA re-segmentation regression: the prefs.tab.* rows were the
    // only exact-match "Appearance"/"Annotate" entries and their deletion left those
    // two untranslated).
    CHECK(std::string(SmatchetLocalization::TranslateSource("General")) == u8"Général");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Appearance")) == u8"Apparence");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Tracker")) == u8"Suivi");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Connections")) == u8"Connexions");
    CHECK(std::string(SmatchetLocalization::TranslateSource("AI & Voice")) == u8"IA et voix");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Editing")) == u8"Édition");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Shortcuts")) == u8"Raccourcis");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Annotate")) == u8"Annoter");
}
