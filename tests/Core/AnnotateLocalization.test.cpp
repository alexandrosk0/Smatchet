// AnnotateLocalization tests — guard the Annotate context-comment labels.
//
// The Blame->Annotate rename left the loc-table English source for these two
// labels lowercase ("Add annotate context comment") while the live UI Selectables
// pass the capitalized feature name ("Add Annotate context comment"). Because
// TranslateSource matches by English source string, the mismatch meant the French
// translation never applied (and the French itself read "contexte annotate" rather
// than proper "contexte d'annotation"). This test pins the corrected mapping so the
// dead-translation regression can't recur.

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

TEST_CASE("Annotate context labels round-trip through TranslateSource") {
    LanguageResetGuard restore;

    // en-US: an English source with no translation returns unchanged.
    SmatchetLocalization::SetLanguage("en-US");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Add Annotate context comment")) ==
          "Add Annotate context comment");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Assign and add Annotate context")) ==
          "Assign and add Annotate context");

    // fr-FR: the capitalized live-UI source strings must now match the loc table and
    // translate with correct grammar ("d'annotation", not the old "contexte annotate").
    SmatchetLocalization::SetLanguage("fr-FR");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Add Annotate context comment")) ==
          u8"Ajouter un commentaire de contexte d'annotation");
    CHECK(std::string(SmatchetLocalization::TranslateSource("Assign and add Annotate context")) ==
          u8"Assigner et ajouter le contexte d'annotation");
}
