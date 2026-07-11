// LocalizationFormatGuard tests — pin the printf conversion-specifier guard that
// gates an attacker-influenceable Locales/<lang>.json override before it reaches a
// vsnprintf-family sink in SmatchetLocalization::Format / TranslateSourceAsFormat
// (SECURITY_AUDIT.md #1 / CPP_CODE_AUDIT.md #7). The guard was extracted from
// SmatchetLocalization.cpp file-statics into the header-only smatchet::l10n unit so
// it can be unit- and fuzz-tested (tests/fuzz/fuzz_locale_format_guard.cpp) in
// isolation; these cases pin the extraction as behaviour-identical and lock the
// security invariant: an override may only be used as the format string when its
// conversion-specifier sequence exactly matches the trusted English literal.

#include "SmatchetLocalizationFormatGuard.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::l10n::ConversionSpecifiers;
using smatchet::l10n::FormatSpecifiersMatch;

TEST_CASE("ConversionSpecifiers extracts each %-token through its conversion char") {
    CHECK(ConversionSpecifiers("Model: %s") == std::vector<std::string>{"%s"});
    CHECK(ConversionSpecifiers("%d/%d (%zu rows)") == std::vector<std::string>{"%d", "%d", "%zu"});
    CHECK(ConversionSpecifiers("%08.2f") == std::vector<std::string>{"%08.2f"}); // flags+width+prec
    CHECK(ConversionSpecifiers("%*d") == std::vector<std::string>{"%*d"});       // '*' width
    CHECK(ConversionSpecifiers("%llu B") == std::vector<std::string>{"%llu"});   // length modifier
}

TEST_CASE("ConversionSpecifiers treats %% as a literal (no token) and tolerates edges") {
    CHECK(ConversionSpecifiers("100%% done").empty());
    CHECK(ConversionSpecifiers("no specifiers here").empty());
    CHECK(ConversionSpecifiers("").empty());
    CHECK(ConversionSpecifiers(nullptr).empty());
    // A truncated trailing specifier is captured as one whole-tail token (never OOB).
    CHECK(ConversionSpecifiers("ends with %") == std::vector<std::string>{"%"});
    CHECK(ConversionSpecifiers("padded %-") == std::vector<std::string>{"%-"});
}

TEST_CASE("FormatSpecifiersMatch is true only for an identical specifier sequence") {
    CHECK(FormatSpecifiersMatch("Model: %s", "Model: %s"));
    CHECK(FormatSpecifiersMatch("Modèle : %s", "Model: %s")); // translated prose, same specifiers
    CHECK(FormatSpecifiersMatch("100%% done", "done"));        // both have zero specifiers
    CHECK(FormatSpecifiersMatch("no specifiers", "also none"));
}

TEST_CASE("FormatSpecifiersMatch rejects an override that adds/changes/removes a specifier") {
    // The exact attack the guard exists to stop: an override injects extra/%n specifiers.
    CHECK_FALSE(FormatSpecifiersMatch("%s %s %n", "Views - %s"));
    CHECK_FALSE(FormatSpecifiersMatch("%d", "%s"));       // changed conversion
    CHECK_FALSE(FormatSpecifiersMatch("%s %s", "%s"));    // added specifier
    CHECK_FALSE(FormatSpecifiersMatch("", "%s"));         // removed specifier
    CHECK_FALSE(FormatSpecifiersMatch("%08.2f", "%f"));   // width/precision differs
}

TEST_CASE("FormatSpecifiersMatch is null-safe both ways") {
    CHECK(FormatSpecifiersMatch(nullptr, nullptr));       // [] == []
    CHECK(FormatSpecifiersMatch(nullptr, "plain"));       // [] == []
    CHECK_FALSE(FormatSpecifiersMatch(nullptr, "%s"));    // [] != ["%s"]
    CHECK_FALSE(FormatSpecifiersMatch("%s", nullptr));    // ["%s"] != []
}
