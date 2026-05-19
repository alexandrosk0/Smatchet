// CoderabbitCommentClassifierPure — phase-2 of the `coderabbit-react-loop`
// plan. Exercises the pure UTF-8 / substring helpers backing the phase-3
// concrete `CoderabbitCommentClassifier`. The doctest rig loads
// `tests/fixtures/coderabbit_comments_sample.json` and asserts each row's
// expected parse against the helpers — every CodeRabbit body shape we
// recognise is locked at this layer.
//
// No HTTP / SQLite / ImGui dependencies — the helpers are pure C++14 and
// the fixture is a flat JSON array. Always-compiled (helpers compile on
// both AGENTIC=ON and AGENTIC=OFF — the dependency on the AGENTIC gate is
// purely the `PrCommentClassifier.h` interface, not the parsers).

#include "CoderabbitCommentClassifierPure.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate tests/fixtures/*"
#endif

using ::smatchet::agentic::coderabbit_pure::ExtractActionableHeaderCount;
using ::smatchet::agentic::coderabbit_pure::ExtractNitpickHeaderCount;
using ::smatchet::agentic::coderabbit_pure::ExtractSeverityIcon;
using ::smatchet::agentic::coderabbit_pure::ExtractSuggestionBlocks;
using ::smatchet::agentic::coderabbit_pure::HasSmatchetHandoffMarker;
using ::smatchet::agentic::coderabbit_pure::IsBotLoginAllowed;
using ::smatchet::agentic::coderabbit_pure::SeverityIcon;

namespace {

// Canonical icon byte sequences — duplicated here as raw escapes so the test
// source is independent of the parser's internal constants. If the parser
// changes which icon it recognises, the test will fail to find the matching
// fixture row, surfacing the drift.
const char* const kIconActionable = "\xF0\x9F\x9B\xA0"; // U+1F6E0 wrench+screwdriver
const char* const kIconCaution = "\xE2\x9A\xA0";        // U+26A0  warning sign
const char* const kIconNit = "\xF0\x9F\x92\xA1";        // U+1F4A1 light bulb
const char* const kIconChore = "\xF0\x9F\xA7\xB9";      // U+1F9F9 broom

const std::string kFixtureDir = std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures";

nlohmann::json LoadFixture() {
    const std::string path = kFixtureDir + "/coderabbit_comments_sample.json";
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.is_open(), ("Fixture file unreadable: " + path).c_str());
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str());
}

SeverityIcon IconFromName(const std::string& name) {
    if (name == "actionable")
        return SeverityIcon::Actionable;
    if (name == "caution")
        return SeverityIcon::Caution;
    if (name == "nit")
        return SeverityIcon::Nit;
    if (name == "chore")
        return SeverityIcon::Chore;
    if (name == "none")
        return SeverityIcon::None;
    // Fail fast on fixture typos — silently coercing unknown values to
    // `SeverityIcon::None` masks drift between the fixture file and the
    // pure-helper enum.
    throw std::invalid_argument("Unknown expected_icon: " + name);
}

} // namespace

TEST_SUITE("CoderabbitCommentClassifierPure") {

    TEST_CASE("ExtractSeverityIcon: each enum value matches the canonical icon") {
        CHECK(ExtractSeverityIcon(std::string(kIconActionable) + " refactor") == SeverityIcon::Actionable);
        CHECK(ExtractSeverityIcon(std::string(kIconCaution) + " careful") == SeverityIcon::Caution);
        CHECK(ExtractSeverityIcon(std::string(kIconNit) + " nit") == SeverityIcon::Nit);
        CHECK(ExtractSeverityIcon(std::string(kIconChore) + " chore") == SeverityIcon::Chore);
    }

    TEST_CASE("ExtractSeverityIcon: empty body returns None") { CHECK(ExtractSeverityIcon("") == SeverityIcon::None); }

    TEST_CASE("ExtractSeverityIcon: plain ASCII body returns None") {
        CHECK(ExtractSeverityIcon("This is a plain comment with no icon at all.") == SeverityIcon::None);
    }

    TEST_CASE("ExtractSeverityIcon: first match wins when multiple icons present") {
        // Nit icon appears first → result is Nit, not Actionable even though both are present.
        std::string body = std::string("prefix ") + kIconNit + " then later " + kIconActionable + " too";
        CHECK(ExtractSeverityIcon(body) == SeverityIcon::Nit);
    }

    TEST_CASE("ExtractSeverityIcon: icon with VS-16 variation selector still matches") {
        // CodeRabbit sometimes emits U+1F6E0 followed by U+FE0F. The parser must
        // still match on the bare codepoint.
        std::string body = std::string(kIconActionable) + "\xEF\xB8\x8F refactor suggestion";
        CHECK(ExtractSeverityIcon(body) == SeverityIcon::Actionable);
    }

    TEST_CASE("ExtractActionableHeaderCount: parses canonical header") {
        CHECK(ExtractActionableHeaderCount("_Actionable comments posted: 3_\n\nrest") == 3);
        CHECK(ExtractActionableHeaderCount("_Actionable comments posted: 0_") == 0);
        CHECK(ExtractActionableHeaderCount("_Actionable comments posted: 17_") == 17);
    }

    TEST_CASE("ExtractActionableHeaderCount: header absent returns -1") {
        CHECK(ExtractActionableHeaderCount("") == -1);
        CHECK(ExtractActionableHeaderCount("just a comment body") == -1);
        CHECK(ExtractActionableHeaderCount("_Nitpick comments (5)_") == -1); // wrong header shape
    }

    TEST_CASE("ExtractActionableHeaderCount: malformed (no number) returns -1") {
        CHECK(ExtractActionableHeaderCount("_Actionable comments posted: _") == -1);
        CHECK(ExtractActionableHeaderCount("_Actionable comments posted: foo_") == -1);
    }

    TEST_CASE("ExtractNitpickHeaderCount: parses canonical bracketed header") {
        CHECK(ExtractNitpickHeaderCount("_Nitpick comments (2)_") == 2);
        CHECK(ExtractNitpickHeaderCount("_Nitpick comments (0)_") == 0);
        CHECK(ExtractNitpickHeaderCount("prefix\n_Nitpick comments (5)_\nrest") == 5);
    }

    TEST_CASE("ExtractNitpickHeaderCount: header absent returns -1") {
        CHECK(ExtractNitpickHeaderCount("") == -1);
        CHECK(ExtractNitpickHeaderCount("_Actionable comments posted: 3_") == -1); // wrong header shape
    }

    TEST_CASE("ExtractNitpickHeaderCount: missing closing paren returns -1") {
        CHECK(ExtractNitpickHeaderCount("_Nitpick comments (5") == -1);
        CHECK(ExtractNitpickHeaderCount("_Nitpick comments (foo)_") == -1);
    }

    TEST_CASE("ExtractSuggestionBlocks: single block extracted") {
        std::string body = "Some prose.\n\n```suggestion\nfoo();\n```\n\nmore prose";
        std::vector<std::string> blocks = ExtractSuggestionBlocks(body);
        REQUIRE(blocks.size() == 1);
        CHECK(blocks[0] == "foo();");
    }

    TEST_CASE("ExtractSuggestionBlocks: empty body yields empty vector") {
        CHECK(ExtractSuggestionBlocks("").empty());
        CHECK(ExtractSuggestionBlocks("body with no fences").empty());
    }

    TEST_CASE("ExtractSuggestionBlocks: two distinct blocks extracted in order") {
        std::string body = "first:\n```suggestion\nA\n```\nthen:\n```suggestion\nB\n```";
        std::vector<std::string> blocks = ExtractSuggestionBlocks(body);
        REQUIRE(blocks.size() == 2);
        CHECK(blocks[0] == "A");
        CHECK(blocks[1] == "B");
    }

    TEST_CASE("ExtractSuggestionBlocks: multi-line block preserves interior newlines") {
        std::string body = "```suggestion\nline1\nline2\nline3\n```";
        std::vector<std::string> blocks = ExtractSuggestionBlocks(body);
        REQUIRE(blocks.size() == 1);
        CHECK(blocks[0] == "line1\nline2\nline3");
    }

    TEST_CASE("ExtractSuggestionBlocks: malformed (no closer) is skipped") {
        std::string body = "```suggestion\nopen-but-no-close\n";
        CHECK(ExtractSuggestionBlocks(body).empty());
    }

    TEST_CASE("ExtractSuggestionBlocks: non-suggestion fenced block is not extracted") {
        // A plain ```cpp ... ``` block must not be captured.
        std::string body = "```cpp\nint x = 0;\n```";
        CHECK(ExtractSuggestionBlocks(body).empty());
    }

    TEST_CASE("IsBotLoginAllowed: exact case-insensitive match passes") {
        std::vector<std::string> allow = {"coderabbitai[bot]"};
        CHECK(IsBotLoginAllowed("coderabbitai[bot]", allow));
        CHECK(IsBotLoginAllowed("CodeRabbitAi[Bot]", allow));
    }

    TEST_CASE("IsBotLoginAllowed: empty inputs fail") {
        std::vector<std::string> allow = {"coderabbitai[bot]"};
        CHECK_FALSE(IsBotLoginAllowed("", allow));
        CHECK_FALSE(IsBotLoginAllowed("coderabbitai[bot]", {}));
        CHECK_FALSE(IsBotLoginAllowed("", {}));
    }

    TEST_CASE("IsBotLoginAllowed: non-match returns false") {
        std::vector<std::string> allow = {"coderabbitai[bot]", "renovate[bot]"};
        CHECK_FALSE(IsBotLoginAllowed("dependabot[bot]", allow));
        CHECK_FALSE(IsBotLoginAllowed("alex", allow));
    }

    TEST_CASE("IsBotLoginAllowed: bot-suffix normalization handles base-name vs bracketed") {
        // Configured base login matches the observed bracketed-bot login —
        // exact-match after stripping a single trailing `[bot]`.
        std::vector<std::string> allow = {"coderabbitai"};
        CHECK(IsBotLoginAllowed("coderabbitai[bot]", allow));
        // Same in the reverse direction (configured bracketed, observed base).
        std::vector<std::string> allowBracketed = {"coderabbitai[bot]"};
        CHECK(IsBotLoginAllowed("coderabbitai", allowBracketed));
    }

    TEST_CASE("IsBotLoginAllowed: prefix/suffix spoofs are rejected") {
        // Substring matches would allow these; exact normalized comparison
        // must reject. Guards against an attacker registering a similarly
        // named bot account to bypass the allow-list.
        std::vector<std::string> allow = {"coderabbitai"};
        CHECK_FALSE(IsBotLoginAllowed("evil-coderabbitai", allow));
        CHECK_FALSE(IsBotLoginAllowed("evil-coderabbitai[bot]", allow));
        CHECK_FALSE(IsBotLoginAllowed("coderabbitai-evil", allow));
        CHECK_FALSE(IsBotLoginAllowed("coderabbitai-evil[bot]", allow));
        // Substring-of-allow-entry is also rejected.
        std::vector<std::string> allowLong = {"coderabbitai-official"};
        CHECK_FALSE(IsBotLoginAllowed("coderabbitai", allowLong));
    }

    TEST_CASE("HasSmatchetHandoffMarker: positive + negative") {
        CHECK(HasSmatchetHandoffMarker("<!-- smatchet-handoff -->\n\nbody"));
        CHECK(HasSmatchetHandoffMarker("prefix\n<!-- smatchet-handoff -->\nrest")); // anywhere in body
        CHECK_FALSE(HasSmatchetHandoffMarker(""));
        CHECK_FALSE(HasSmatchetHandoffMarker("regular CodeRabbit comment"));
        CHECK_FALSE(HasSmatchetHandoffMarker("<!-- other-marker -->"));
    }

    TEST_CASE("Fixture round-trip: every row's parsed icon matches expected_icon") {
        const nlohmann::json fixture = LoadFixture();
        REQUIRE(fixture.is_array());
        // Lock the documented 8-row fixture exactly — a row drop should
        // surface here, not after the loop silently iterates fewer cases.
        REQUIRE(fixture.size() == 8);

        for (const auto& row : fixture) {
            const std::string id = row.at("id").get<std::string>();
            const std::string body = row.at("body").get<std::string>();
            const std::string expectedIconName = row.at("expected_icon").get<std::string>();
            const SeverityIcon expected = IconFromName(expectedIconName);
            const SeverityIcon actual = ExtractSeverityIcon(body);
            const std::string msg = "row id=" + id + " expected icon " + expectedIconName;
            CHECK_MESSAGE(actual == expected, msg.c_str());
        }
    }

    TEST_CASE("Fixture round-trip: suggestion-block count matches expected") {
        const nlohmann::json fixture = LoadFixture();
        for (const auto& row : fixture) {
            const std::string id = row.at("id").get<std::string>();
            const std::string body = row.at("body").get<std::string>();
            const std::size_t expected = row.at("expected_suggestion_blocks").get<std::size_t>();
            const std::vector<std::string> blocks = ExtractSuggestionBlocks(body);
            const std::string msg = "row id=" + id + " expected " + std::to_string(expected) +
                                    " suggestion-blocks; got " + std::to_string(blocks.size());
            CHECK_MESSAGE(blocks.size() == expected, msg.c_str());
        }
    }

    TEST_CASE("Fixture round-trip: smatchet-handoff marker detection") {
        const nlohmann::json fixture = LoadFixture();
        for (const auto& row : fixture) {
            const std::string id = row.at("id").get<std::string>();
            const std::string body = row.at("body").get<std::string>();
            const bool expected = row.at("expected_marker").get<bool>();
            const std::string msg = "row id=" + id + " expected marker=" + (expected ? "true" : "false");
            CHECK_MESSAGE(HasSmatchetHandoffMarker(body) == expected, msg.c_str());
        }
    }

    TEST_CASE("Fixture round-trip: actionable + nitpick header counts on summary row") {
        const nlohmann::json fixture = LoadFixture();
        bool sawSummaryRow = false;
        for (const auto& row : fixture) {
            if (!row.contains("expected_actionable_count"))
                continue;
            if (!row.contains("expected_nitpick_count"))
                continue;
            sawSummaryRow = true;
            const std::string id = row.at("id").get<std::string>();
            const std::string body = row.at("body").get<std::string>();
            const int expectedActionable = row.at("expected_actionable_count").get<int>();
            const int expectedNitpick = row.at("expected_nitpick_count").get<int>();
            const std::string msgA = "row id=" + id + " actionable count mismatch";
            const std::string msgN = "row id=" + id + " nitpick count mismatch";
            CHECK_MESSAGE(ExtractActionableHeaderCount(body) == expectedActionable, msgA.c_str());
            CHECK_MESSAGE(ExtractNitpickHeaderCount(body) == expectedNitpick, msgN.c_str());
        }
        CHECK_MESSAGE(sawSummaryRow, "fixture must include at least one summary-header row");
    }

} // TEST_SUITE
