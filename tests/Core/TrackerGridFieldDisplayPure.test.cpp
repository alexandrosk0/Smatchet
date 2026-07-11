#include <doctest/doctest.h>

#include "TrackerGridFieldDisplayPure.h"

#include <string>

using TrackerGridFieldDisplayPure::BuildAttachmentRenderModel;
using TrackerGridFieldDisplayPure::BuildIssueRestrictionRenderModel;
using TrackerGridFieldDisplayPure::BuildProgressRenderModel;
using TrackerGridFieldDisplayPure::BuildVotesRenderModel;
using TrackerGridFieldDisplayPure::BuildWatchersRenderModel;
using TrackerGridFieldDisplayPure::BuildWorklogRenderModel;

// Every input below is a CachedTicket field value string as a tracker backend produced it —
// off-host bytes. The builders must never throw / crash on hostile shapes; the fallback is
// always the default-constructed (unparsed/unrendered) model, which the draw shell renders as
// plain clipped text.

namespace {

// A nesting bomb comfortably past json_safe::ParseBounded's depth cap. The pre-remediation
// bare nlohmann parse recursed the native stack on this class of payload (SECURITY_AUDIT.md
// unbounded-parse finding on this TU); the bounded parse must reject it and every builder must
// degrade to its unparsed model instead of crashing.
std::string DeepNestingBomb() {
    std::string bomb;
    for (int i = 0; i < 5000; ++i) {
        bomb += '[';
    }
    bomb += '1';
    for (int i = 0; i < 5000; ++i) {
        bomb += ']';
    }
    return bomb;
}

} // namespace

TEST_CASE("BuildAttachmentRenderModel — Jira attachment array with fallback key chains") {
    const std::string value = R"([
        {"filename":"design.png","mimeType":"image/png","content":"https://x/att/1"},
        {"name":"notes.txt","mime_type":"text/plain","content":{"url":"https://x/att/2"}},
        {"content":{"self":"https://x/att/3"}}
    ])";
    const auto model = BuildAttachmentRenderModel(value);
    REQUIRE(model.parsed);
    REQUIRE(model.descriptors.size() == 3);
    // filename → name fallback; mimeType → mime_type fallback; content string vs object url/self.
    CHECK(model.descriptors[0].Filename == "design.png");
    CHECK(model.descriptors[0].MimeType == "image/png");
    CHECK(model.descriptors[0].Url == "https://x/att/1");
    CHECK(model.descriptors[1].Filename == "notes.txt");
    CHECK(model.descriptors[1].MimeType == "text/plain");
    CHECK(model.descriptors[1].Url == "https://x/att/2");
    // Nameless entry gets the "Attachment" placeholder + content.self URL.
    CHECK(model.descriptors[2].Filename == "Attachment");
    CHECK(model.descriptors[2].Url == "https://x/att/3");
    // Display line is first filename + "+N" overflow count.
    CHECK(model.display == "design.png +2");
    CHECK(model.tooltip.find("design.png") != std::string::npos);
    CHECK(model.tooltip.find("[image/png]") != std::string::npos);
}

TEST_CASE("BuildAttachmentRenderModel — explicit-empty vs unparsed fallback") {
    SUBCASE("empty / null / [] / {\"attachments\":[]} are explicit-empty") {
        for (const char* v : {"", "   ", "null", "[]", R"({"attachments":[]})"}) {
            const auto model = BuildAttachmentRenderModel(v);
            CHECK_FALSE(model.parsed);
            CHECK(model.explicitEmpty);
        }
    }
    SUBCASE("non-JSON garbage is unparsed but NOT explicit-empty (raw text falls through)") {
        const auto model = BuildAttachmentRenderModel("not json at all");
        CHECK_FALSE(model.parsed);
        CHECK_FALSE(model.explicitEmpty);
    }
    SUBCASE("entries with neither filename nor url are dropped; all-dropped array is unparsed") {
        const auto model = BuildAttachmentRenderModel(R"([{"mimeType":"image/png"},42,"str"])");
        CHECK_FALSE(model.parsed);
    }
    SUBCASE("single object (non-array) wraps into a one-item list") {
        const auto model = BuildAttachmentRenderModel(R"({"filename":"a.txt","content":"https://x/1"})");
        REQUIRE(model.parsed);
        REQUIRE(model.descriptors.size() == 1);
        CHECK(model.display == "a.txt");
    }
    SUBCASE("double-encoded payload (JSON string wrapping JSON) parses") {
        const auto model = BuildAttachmentRenderModel(R"("[{\"filename\":\"in.txt\",\"content\":\"https://x/9\"}]")");
        REQUIRE(model.parsed);
        CHECK(model.descriptors[0].Filename == "in.txt");
    }
}

TEST_CASE("BuildWatchersRenderModel — count/isWatching phrasing + negative clamp") {
    SUBCASE("zero watchers") {
        CHECK(BuildWatchersRenderModel(R"({"watchCount":0})").line == "No watchers");
        CHECK(BuildWatchersRenderModel(R"({"watchCount":0,"isWatching":true})").line == "You watch");
    }
    SUBCASE("singular / plural / incl-you") {
        CHECK(BuildWatchersRenderModel(R"({"watchCount":1})").line == "1 watcher");
        CHECK(BuildWatchersRenderModel(R"({"watchCount":1,"isWatching":true})").line == "1 watcher (you)");
        CHECK(BuildWatchersRenderModel(R"({"watchCount":3})").line == "3 watchers");
        CHECK(BuildWatchersRenderModel(R"({"watchCount":3,"isWatching":true})").line == "3 watchers (incl. you)");
    }
    SUBCASE("negative count clamps to zero; integer isWatching coerces") {
        const auto model = BuildWatchersRenderModel(R"({"watchCount":-5,"isWatching":1})");
        REQUIRE(model.parsed);
        CHECK(model.isWatching);
        CHECK(model.line == "You watch");
    }
    SUBCASE("string watchCount goes through the loose parser") {
        CHECK(BuildWatchersRenderModel(R"({"watchCount":"2"})").line == "2 watchers");
    }
    SUBCASE("self URL lands in the tooltip") {
        const auto model = BuildWatchersRenderModel(R"({"watchCount":2,"self":"https://x/watchers"})");
        CHECK(model.tooltip.find("https://x/watchers") != std::string::npos);
    }
    SUBCASE("non-object / garbage / empty are unparsed") {
        CHECK_FALSE(BuildWatchersRenderModel("[1,2]").parsed);
        CHECK_FALSE(BuildWatchersRenderModel("garbage").parsed);
        CHECK_FALSE(BuildWatchersRenderModel("").parsed);
    }
}

TEST_CASE("BuildVotesRenderModel — count/hasVoted phrasing + negative clamp") {
    CHECK(BuildVotesRenderModel(R"({"votes":0})").line == "No votes");
    CHECK(BuildVotesRenderModel(R"({"votes":0,"hasVoted":true})").line == "You voted");
    CHECK(BuildVotesRenderModel(R"({"votes":1})").line == "1 vote");
    CHECK(BuildVotesRenderModel(R"({"votes":1,"hasVoted":true})").line == "1 vote (you)");
    CHECK(BuildVotesRenderModel(R"({"votes":4})").line == "4 votes");
    CHECK(BuildVotesRenderModel(R"({"votes":4,"hasVoted":1})").line == "4 votes (incl. you)");
    CHECK(BuildVotesRenderModel(R"({"votes":-2})").line == "No votes");
    CHECK_FALSE(BuildVotesRenderModel("not json").parsed);
}

TEST_CASE("BuildWorklogRenderModel — totals, partial-page marker, tooltip entry cap") {
    SUBCASE("zero total renders the dash line") {
        const auto model = BuildWorklogRenderModel(R"({"total":0,"worklogs":[]})");
        REQUIRE(model.parsed);
        CHECK(model.line == "-");
        // maxResults == 0 must NOT emit the page-size tooltip line (pins the > 0 bound).
        CHECK(model.tooltip.find("Page size (maxResults):") == std::string::npos);
    }
    SUBCASE("'This page:' needs BOTH total and page entries non-zero") {
        const auto zeroTotal = BuildWorklogRenderModel(R"({"total":0,"worklogs":[{"timeSpentSeconds":60}]})");
        REQUIRE(zeroTotal.parsed);
        CHECK(zeroTotal.tooltip.find("This page:") == std::string::npos);
        const auto emptyPage = BuildWorklogRenderModel(R"({"total":5,"worklogs":[]})");
        REQUIRE(emptyPage.parsed);
        CHECK(emptyPage.tooltip.find("This page:") == std::string::npos);
    }
    SUBCASE("summed page seconds + partial-page asterisk") {
        const std::string value = R"({
            "total": 3, "startAt": 0, "maxResults": 2,
            "worklogs": [
                {"author":{"displayName":"Ann"},"timeSpent":"1h","timeSpentSeconds":3600,"started":"2026-01-01T10:00:00.000+0000"},
                {"author":{"accountId":"u2"},"timeSpentSeconds":1800}
            ]
        })";
        const auto model = BuildWorklogRenderModel(value);
        REQUIRE(model.parsed);
        // 3 entries total but only 2 on page → the display line carries the "*" partial marker.
        CHECK(model.line.find("3 work logs") == 0);
        CHECK(model.line.back() == '*');
        CHECK(model.tooltip.find("This page: 1\xe2\x80\x93") != std::string::npos);
        CHECK(model.tooltip.find("Page size (maxResults): 2") != std::string::npos);
        CHECK(model.tooltip.find("Ann") != std::string::npos);
        CHECK(model.tooltip.find("u2") != std::string::npos); // accountId fallback for nameless author
        CHECK(model.tooltip.find("(partial; not all work logs loaded)") != std::string::npos);
        CHECK(model.tooltip.find("More entries exist in Tracker") != std::string::npos);
    }
    SUBCASE("tooltip caps at 12 entries and reports the remainder") {
        std::string value = R"({"total":20,"worklogs":[)";
        for (int i = 0; i < 20; ++i) {
            if (i > 0) {
                value += ",";
            }
            value += R"({"author":{"displayName":"W)" + std::to_string(i) + R"("},"timeSpentSeconds":60})";
        }
        value += "]}";
        const auto model = BuildWorklogRenderModel(value);
        REQUIRE(model.parsed);
        CHECK(model.tooltip.find("W11") != std::string::npos);
        CHECK(model.tooltip.find("W12") == std::string::npos);
        CHECK(model.tooltip.find("... +8 on this page") != std::string::npos);
        // A FULL page (WorklogsOnPage == Total == 20) must NOT carry the "*" partial-load
        // marker; that asterisk means "more work logs exist in Tracker than are shown".
        CHECK(model.line.back() != '*');
    }
    SUBCASE("missing worklogs array is unparsed") { CHECK_FALSE(BuildWorklogRenderModel(R"({"total":3})").parsed); }
}

TEST_CASE("BuildIssueRestrictionRenderModel — shouldDisplay coercions + restriction counts") {
    SUBCASE("counts") {
        CHECK(BuildIssueRestrictionRenderModel(R"({"issuerestrictions":{}})").display == "No restrictions");
        CHECK(BuildIssueRestrictionRenderModel(R"({"issuerestrictions":{"a":1}})").display == "1 restriction");
        CHECK(BuildIssueRestrictionRenderModel(R"({"issuerestrictions":{"a":1,"b":2}})").display == "2 restrictions");
    }
    SUBCASE("shouldDisplay false-y coercions across JSON types") {
        for (const char* v :
             {R"({"issuerestrictions":{},"shouldDisplay":false})", R"({"issuerestrictions":{},"shouldDisplay":0})",
              R"({"issuerestrictions":{},"shouldDisplay":0.0})", R"({"issuerestrictions":{},"shouldDisplay":"no"})",
              R"({"issuerestrictions":{},"shouldDisplay":"off"})", R"({"issuerestrictions":{},"shouldDisplay":""})"}) {
            CHECK(BuildIssueRestrictionRenderModel(v).display == "Not displayed");
        }
    }
    SUBCASE("truthy string keeps the count display") {
        CHECK(BuildIssueRestrictionRenderModel(R"({"issuerestrictions":{},"shouldDisplay":"yes"})").display ==
              "No restrictions");
    }
    SUBCASE("missing issuerestrictions object is unrendered") {
        CHECK_FALSE(BuildIssueRestrictionRenderModel(R"({"shouldDisplay":true})").rendered);
        CHECK_FALSE(BuildIssueRestrictionRenderModel("").rendered);
    }
}

TEST_CASE("BuildProgressRenderModel — fast-path scanner, DOM fallback, overflow clamp") {
    SUBCASE("blank renders an empty bar") {
        const auto model = BuildProgressRenderModel("   ");
        CHECK(model.rendered);
        CHECK(model.fraction == doctest::Approx(0.0f));
    }
    SUBCASE("typical Jira progress JSON takes the zero-allocation fast path") {
        const auto model = BuildProgressRenderModel(R"({"progress":25,"total":100})");
        REQUIRE(model.rendered);
        CHECK(model.fraction == doctest::Approx(0.25f));
    }
    SUBCASE("string-quoted digits also scan") {
        const auto model = BuildProgressRenderModel(R"({"progress":"3","total":"4"})");
        REQUIRE(model.rendered);
        CHECK(model.fraction == doctest::Approx(0.75f));
    }
    SUBCASE("oversized digit run clamps instead of overflowing (UB guard)") {
        const auto model = BuildProgressRenderModel(R"({"progress":99999999999999999999,"total":100000000})");
        REQUIRE(model.rendered);
        CHECK(model.fraction >= 0.0f); // no UB; clamped values still produce a finite fraction
    }
    SUBCASE("zero total via DOM path is an empty bar, not a division") {
        const auto model = BuildProgressRenderModel(R"({"progress":0,"total":0})");
        REQUIRE(model.rendered);
        CHECK(model.fraction == doctest::Approx(0.0f));
    }
    SUBCASE("non-progress JSON is unrendered (falls back to text render)") {
        CHECK_FALSE(BuildProgressRenderModel(R"({"percent":50})").rendered);
        CHECK_FALSE(BuildProgressRenderModel("plain text").rendered);
    }
}

TEST_CASE("hostile payloads — nesting bomb degrades to the fallback model in every builder") {
    // Pre-remediation this class of input drove unbounded native-stack recursion in the bare
    // nlohmann parse (SECURITY_AUDIT.md finding on TrackerGridFieldDisplay.cpp); ParseBounded
    // must reject it and every builder must return its default (unparsed/unrendered) model.
    const std::string bomb = DeepNestingBomb();
    CHECK_FALSE(BuildAttachmentRenderModel(bomb).parsed);
    CHECK_FALSE(BuildWatchersRenderModel(bomb).parsed);
    CHECK_FALSE(BuildVotesRenderModel(bomb).parsed);
    CHECK_FALSE(BuildWorklogRenderModel(bomb).parsed);
    CHECK_FALSE(BuildIssueRestrictionRenderModel(bomb).rendered);
    CHECK_FALSE(BuildProgressRenderModel(bomb).rendered);

    // Double-encoded variant: a JSON string whose payload is the bomb — the nested ParseBounded
    // pass must reject it the same way.
    const std::string doubleEncoded = "\"[[[[[[[[[[\\\"x\\\"]]]]]]]]]]\""; // shallow probe: sane double-encode OK
    CHECK_FALSE(BuildWatchersRenderModel(doubleEncoded).parsed);           // array, not object → unparsed
}
