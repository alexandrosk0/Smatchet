// PlaneCommentMappingPure doctest — pure JSON array → TrackerIssueComment
// mapping. issue-comments PR-C. Mirrors GitHubCommentMappingPure.test.cpp style
// (bare include, smatchet::plane namespace, Pillar-3 tolerance cases) adapted to
// the Plane comment shape: stringly-typed `id`, `actor_detail.display_name` /
// `created_by` author, plain-text `comment_stripped` body.

#include "PlaneCommentMappingPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace smatchet::plane;

// 2021-01-01T00:00:00Z — parsed by ParseIso8601ToUnixSec via _mkgmtime (UTC,
// timezone-independent). 2021-01-02T00:00:00Z is exactly +86400s later.
static const std::int64_t kEpoch2021_01_01 = 1609459200;
static const std::int64_t kEpoch2021_01_02 = 1609545600;

TEST_CASE("MapPlaneIssueComments — happy path maps id/author/stripped body + parses timestamps") {
    nlohmann::json arr = nlohmann::json::array();

    nlohmann::json c0 = nlohmann::json::object();
    c0["id"] = "00000000-0000-0000-0000-000000001001";
    c0["actor_detail"] = nlohmann::json::object();
    c0["actor_detail"]["display_name"] = "ada";
    c0["comment_stripped"] = "first comment";
    c0["comment_html"] = "<p>first comment</p>";
    c0["created_at"] = "2021-01-01T00:00:00Z";
    c0["updated_at"] = "2021-01-01T00:00:00Z";
    arr.push_back(c0);

    nlohmann::json c1 = nlohmann::json::object();
    c1["id"] = "00000000-0000-0000-0000-000000002002";
    c1["actor_detail"] = nlohmann::json::object();
    c1["actor_detail"]["display_name"] = "grace";
    c1["comment_stripped"] = "second comment";
    c1["created_at"] = "2021-01-02T00:00:00Z";
    c1["updated_at"] = "2021-01-02T00:00:00Z";
    arr.push_back(c1);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 2);

    CHECK(out[0].Id == "00000000-0000-0000-0000-000000001001");
    CHECK(out[0].Author == "ada");
    CHECK(out[0].Body == "first comment");
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);

    CHECK(out[1].Id == "00000000-0000-0000-0000-000000002002");
    CHECK(out[1].Author == "grace");
    CHECK(out[1].Body == "second comment");
    CHECK(out[1].CreatedAtSec == kEpoch2021_01_02);
    CHECK(out[1].UpdatedAtSec == kEpoch2021_01_02);
}

TEST_CASE("MapPlaneIssueComments — Body comes from comment_stripped, never comment_html") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x1";
    c["comment_stripped"] = "plain text only";
    c["comment_html"] = "<b>rich</b> markup";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Body == "plain text only");
    CHECK(out[0].Body.find('<') == std::string::npos);
}

TEST_CASE("MapPlaneIssueComments — absent actor_detail falls back to created_by") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x2";
    c["comment_stripped"] = "no actor object";
    c["created_by"] = "creator-uuid-1234";
    // no actor_detail
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author == "creator-uuid-1234");
}

TEST_CASE("MapPlaneIssueComments — actor_detail present but display_name missing falls back to created_by") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x3";
    c["comment_stripped"] = "empty display name";
    c["actor_detail"] = nlohmann::json::object(); // present but no display_name
    c["created_by"] = "fallback-uuid";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author == "fallback-uuid");
}

TEST_CASE("MapPlaneIssueComments — no author fields at all yields empty Author") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x4";
    c["comment_stripped"] = "anon";
    // no actor_detail, no created_by
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author.empty());
    CHECK(out[0].Body == "anon");
}

TEST_CASE("MapPlaneIssueComments — absent updated_at defaults UpdatedAtSec to CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x5";
    c["comment_stripped"] = "no update field";
    c["created_at"] = "2021-01-01T00:00:00Z";
    // no updated_at
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == out[0].CreatedAtSec);
}

TEST_CASE("MapPlaneIssueComments — later updated_at yields UpdatedAtSec > CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x6";
    c["comment_stripped"] = "edited later";
    c["created_at"] = "2021-01-01T00:00:00Z";
    c["updated_at"] = "2021-01-02T00:00:00Z";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_02);
    CHECK(out[0].UpdatedAtSec > out[0].CreatedAtSec);
}

TEST_CASE("MapPlaneIssueComments — missing comment_stripped yields empty Body") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x7";
    c["actor_detail"] = nlohmann::json::object();
    c["actor_detail"]["display_name"] = "ada";
    // no comment_stripped
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Body.empty());
    CHECK(out[0].Author == "ada");
}

TEST_CASE("MapPlaneIssueComments — missing id yields empty Id") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    // no id
    c["comment_stripped"] = "no id";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Id.empty());
}

TEST_CASE("MapPlaneIssueComments — non-string id (number/null) yields empty Id") {
    SUBCASE("number id") {
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json c = nlohmann::json::object();
        c["id"] = 12345;
        c["comment_stripped"] = "x";
        arr.push_back(c);
        const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
        REQUIRE(out.size() == 1);
        CHECK(out[0].Id.empty());
    }
    SUBCASE("null id") {
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json c = nlohmann::json::object();
        c["id"] = nullptr;
        c["comment_stripped"] = "x";
        arr.push_back(c);
        const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
        REQUIRE(out.size() == 1);
        CHECK(out[0].Id.empty());
    }
}

TEST_CASE("MapPlaneIssueComments — malformed timestamps leave epochs at 0 without throwing") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x8";
    c["comment_stripped"] = "bad times";
    c["created_at"] = "not-a-date";
    c["updated_at"] = "also-garbage";
    arr.push_back(c);

    std::vector<TrackerIssueComment> out;
    CHECK_NOTHROW(out = MapPlaneIssueComments(arr));
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == 0);
    CHECK(out[0].UpdatedAtSec == 0);
}

TEST_CASE("MapPlaneIssueComments — bad updated_at falls back to a valid CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = "x9";
    c["comment_stripped"] = "good created, bad updated";
    c["created_at"] = "2021-01-01T00:00:00Z";
    c["updated_at"] = "garbage";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);
}

TEST_CASE("MapPlaneIssueComments — non-array input yields an empty vector (Pillar 3)") {
    SUBCASE("object") {
        nlohmann::json obj = nlohmann::json::object();
        obj["id"] = "1";
        CHECK(MapPlaneIssueComments(obj).empty());
    }
    SUBCASE("null") {
        nlohmann::json n = nlohmann::json();
        CHECK(MapPlaneIssueComments(n).empty());
    }
    SUBCASE("number") {
        nlohmann::json num = 42;
        CHECK(MapPlaneIssueComments(num).empty());
    }
    SUBCASE("string") {
        nlohmann::json s = "comments";
        CHECK(MapPlaneIssueComments(s).empty());
    }
}

TEST_CASE("MapPlaneIssueComments — empty array yields an empty vector") {
    nlohmann::json arr = nlohmann::json::array();
    CHECK(MapPlaneIssueComments(arr).empty());
}

TEST_CASE("MapPlaneIssueComments — non-object array elements are skipped, others mapped") {
    nlohmann::json arr = nlohmann::json::array();

    nlohmann::json good0 = nlohmann::json::object();
    good0["id"] = "100";
    good0["actor_detail"] = nlohmann::json::object();
    good0["actor_detail"]["display_name"] = "alice";
    good0["comment_stripped"] = "real one";
    arr.push_back(good0);

    arr.push_back(nlohmann::json(7));        // number — skipped
    arr.push_back(nlohmann::json("scalar")); // string — skipped
    arr.push_back(nlohmann::json::array());  // nested array — skipped
    arr.push_back(nlohmann::json());         // null — skipped

    nlohmann::json good1 = nlohmann::json::object();
    good1["id"] = "200";
    good1["actor_detail"] = nlohmann::json::object();
    good1["actor_detail"]["display_name"] = "bob";
    good1["comment_stripped"] = "another real one";
    arr.push_back(good1);

    const std::vector<TrackerIssueComment> out = MapPlaneIssueComments(arr);
    REQUIRE(out.size() == 2);
    CHECK(out[0].Id == "100");
    CHECK(out[0].Author == "alice");
    CHECK(out[1].Id == "200");
    CHECK(out[1].Author == "bob");
}
