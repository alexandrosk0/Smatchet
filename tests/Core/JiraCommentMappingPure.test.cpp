// JiraCommentMappingPure doctest — pure Jira comment-node array →
// TrackerIssueComment mapping. issue-comments PR-B. Mirrors the
// GitHubCommentMappingPure.test.cpp style (bare include, Pillar-3 tolerance
// cases) but for the Jira node shape: string `id`, `author.displayName`
// (defaults "Unknown"), ADF/string `body`, ms-precision `created`/`updated`.

#include "JiraCommentMappingPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace smatchet::jira;

// 2021-01-01T00:00:00Z and exactly +86400s later. Jira stamps with millisecond
// precision + a numeric offset ("...:00.000+0000") which ParseIso8601ToUnixSec
// strips before classifying the timezone, so these match the GitHub `Z` epochs.
static const std::int64_t kEpoch2021_01_01 = 1609459200;
static const std::int64_t kEpoch2021_01_02 = 1609545600;

// Build a minimal ADF body wrapping a single paragraph of plain text.
static nlohmann::json AdfParagraph(const std::string& text) {
    nlohmann::json textNode = nlohmann::json::object();
    textNode["type"] = "text";
    textNode["text"] = text;

    nlohmann::json paragraph = nlohmann::json::object();
    paragraph["type"] = "paragraph";
    paragraph["content"] = nlohmann::json::array();
    paragraph["content"].push_back(textNode);

    nlohmann::json doc = nlohmann::json::object();
    doc["type"] = "doc";
    doc["version"] = 1;
    doc["content"] = nlohmann::json::array();
    doc["content"].push_back(paragraph);
    return doc;
}

TEST_CASE("MapJiraIssueComments — happy path maps id/author/ADF-body + parses timestamps") {
    nlohmann::json arr = nlohmann::json::array();

    nlohmann::json c0 = nlohmann::json::object();
    c0["id"] = "1001";
    c0["author"] = nlohmann::json::object();
    c0["author"]["displayName"] = "Ada Lovelace";
    c0["body"] = AdfParagraph("first comment");
    c0["created"] = "2021-01-01T00:00:00.000+0000";
    c0["updated"] = "2021-01-01T00:00:00.000+0000";
    arr.push_back(c0);

    nlohmann::json c1 = nlohmann::json::object();
    c1["id"] = "2002";
    c1["author"] = nlohmann::json::object();
    c1["author"]["displayName"] = "Grace Hopper";
    c1["body"] = AdfParagraph("second comment");
    c1["created"] = "2021-01-02T00:00:00.000+0000";
    c1["updated"] = "2021-01-02T00:00:00.000+0000";
    arr.push_back(c1);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 2);

    CHECK(out[0].Id == "1001");
    CHECK(out[0].Author == "Ada Lovelace");
    CHECK(out[0].Body == "first comment");
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);

    CHECK(out[1].Id == "2002");
    CHECK(out[1].Author == "Grace Hopper");
    CHECK(out[1].Body == "second comment");
    CHECK(out[1].CreatedAtSec == kEpoch2021_01_02);
    CHECK(out[1].UpdatedAtSec == kEpoch2021_01_02);
}

TEST_CASE("MapJiraIssueComments — plain-string body passes through unchanged") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "3";
    c["body"] = "legacy wiki-markup body";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Body == "legacy wiki-markup body");
}

TEST_CASE("MapJiraIssueComments — absent author defaults Author to \"Unknown\"") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "4";
    c["body"] = AdfParagraph("anon");
    // no author
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author == "Unknown");
    CHECK(out[0].Body == "anon");
}

TEST_CASE("MapJiraIssueComments — author object without displayName defaults to \"Unknown\"") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "5";
    c["author"] = nlohmann::json::object(); // present but no displayName
    c["body"] = AdfParagraph("x");
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author == "Unknown");
}

TEST_CASE("MapJiraIssueComments — absent updated defaults UpdatedAtSec to CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "6";
    c["body"] = AdfParagraph("no update field");
    c["created"] = "2021-01-01T00:00:00.000+0000";
    // no updated
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == out[0].CreatedAtSec);
}

TEST_CASE("MapJiraIssueComments — later updated yields UpdatedAtSec > CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "7";
    c["body"] = AdfParagraph("edited later");
    c["created"] = "2021-01-01T00:00:00.000+0000";
    c["updated"] = "2021-01-02T00:00:00.000+0000";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_02);
    CHECK(out[0].UpdatedAtSec > out[0].CreatedAtSec);
}

TEST_CASE("MapJiraIssueComments — missing body yields empty Body") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "8";
    c["author"] = nlohmann::json::object();
    c["author"]["displayName"] = "Ada";
    // no body
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Body.empty());
    CHECK(out[0].Author == "Ada");
}

TEST_CASE("MapJiraIssueComments — ADF object without content yields empty Body") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "9";
    c["body"] = nlohmann::json::object(); // object but no `content`
    c["body"]["type"] = "doc";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Body.empty());
}

TEST_CASE("MapJiraIssueComments — non-string id (number/null) yields empty Id") {
    SUBCASE("number id") {
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json c = nlohmann::json::object();
        c["id"] = 1001; // numeric, not a string — Jira uses strings
        c["body"] = "x";
        arr.push_back(c);
        const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
        REQUIRE(out.size() == 1);
        CHECK(out[0].Id.empty());
    }
    SUBCASE("null id") {
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json c = nlohmann::json::object();
        c["id"] = nullptr;
        c["body"] = "x";
        arr.push_back(c);
        const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
        REQUIRE(out.size() == 1);
        CHECK(out[0].Id.empty());
    }
}

TEST_CASE("MapJiraIssueComments — malformed timestamps leave epochs at 0 without throwing") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "10";
    c["body"] = "bad times";
    c["created"] = "not-a-date";
    c["updated"] = "also-garbage";
    arr.push_back(c);

    std::vector<TrackerIssueComment> out;
    CHECK_NOTHROW(out = MapJiraIssueComments(arr));
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == 0);
    CHECK(out[0].UpdatedAtSec == 0);
}

TEST_CASE("MapJiraIssueComments — bad updated falls back to a valid CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "11";
    c["body"] = "good created, bad updated";
    c["created"] = "2021-01-01T00:00:00.000+0000";
    c["updated"] = "garbage";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);
}

TEST_CASE("MapJiraIssueComments — non-array input yields an empty vector (Pillar 3)") {
    SUBCASE("object") {
        nlohmann::json obj = nlohmann::json::object();
        obj["id"] = "1";
        CHECK(MapJiraIssueComments(obj).empty());
    }
    SUBCASE("null") {
        nlohmann::json n = nlohmann::json();
        CHECK(MapJiraIssueComments(n).empty());
    }
    SUBCASE("number") {
        nlohmann::json num = 42;
        CHECK(MapJiraIssueComments(num).empty());
    }
    SUBCASE("string") {
        nlohmann::json s = "comments";
        CHECK(MapJiraIssueComments(s).empty());
    }
}

TEST_CASE("MapJiraIssueComments — empty array yields an empty vector") {
    nlohmann::json arr = nlohmann::json::array();
    CHECK(MapJiraIssueComments(arr).empty());
}

TEST_CASE("MapJiraIssueComments — non-object array elements are skipped, others mapped") {
    nlohmann::json arr = nlohmann::json::array();

    nlohmann::json good0 = nlohmann::json::object();
    good0["id"] = "100";
    good0["author"] = nlohmann::json::object();
    good0["author"]["displayName"] = "alice";
    good0["body"] = AdfParagraph("real one");
    arr.push_back(good0);

    arr.push_back(nlohmann::json(7));        // number — skipped
    arr.push_back(nlohmann::json("scalar")); // string — skipped
    arr.push_back(nlohmann::json::array());  // nested array — skipped
    arr.push_back(nlohmann::json());         // null — skipped

    nlohmann::json good1 = nlohmann::json::object();
    good1["id"] = "200";
    good1["author"] = nlohmann::json::object();
    good1["author"]["displayName"] = "bob";
    good1["body"] = AdfParagraph("another real one");
    arr.push_back(good1);

    const std::vector<TrackerIssueComment> out = MapJiraIssueComments(arr);
    REQUIRE(out.size() == 2);
    CHECK(out[0].Id == "100");
    CHECK(out[0].Author == "alice");
    CHECK(out[1].Id == "200");
    CHECK(out[1].Author == "bob");
}
