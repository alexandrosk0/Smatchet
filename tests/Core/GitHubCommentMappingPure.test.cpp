// GitHubCommentMappingPure doctest — pure JSON array → TrackerIssueComment
// mapping. issue-comments PR-A. Mirrors the GitHubIssueSearchMapping.test.cpp
// style (bare include, smatchet::github namespace, Pillar-3 tolerance cases).

#include "GitHubCommentMappingPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace smatchet::github;

// 2021-01-01T00:00:00Z — captured by running ParseIso8601ToUnixSec via _mkgmtime
// (UTC, timezone-independent). 2021-01-02T00:00:00Z is exactly +86400s later.
static const std::int64_t kEpoch2021_01_01 = 1609459200;
static const std::int64_t kEpoch2021_01_02 = 1609545600;

TEST_CASE("MapGitHubIssueComments — happy path maps id/login/body + parses timestamps") {
    nlohmann::json arr = nlohmann::json::array();

    nlohmann::json c0 = nlohmann::json::object();
    c0["id"] = 1001;
    c0["user"] = nlohmann::json::object();
    c0["user"]["login"] = "octocat";
    c0["body"] = "first comment";
    c0["created_at"] = "2021-01-01T00:00:00Z";
    c0["updated_at"] = "2021-01-01T00:00:00Z";
    arr.push_back(c0);

    nlohmann::json c1 = nlohmann::json::object();
    c1["id"] = 2002;
    c1["user"] = nlohmann::json::object();
    c1["user"]["login"] = "hubber";
    c1["body"] = "second comment";
    c1["created_at"] = "2021-01-02T00:00:00Z";
    c1["updated_at"] = "2021-01-02T00:00:00Z";
    arr.push_back(c1);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 2);

    CHECK(out[0].Id == "1001");
    CHECK(out[0].Author == "octocat");
    CHECK(out[0].Body == "first comment");
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);

    CHECK(out[1].Id == "2002");
    CHECK(out[1].Author == "hubber");
    CHECK(out[1].Body == "second comment");
    CHECK(out[1].CreatedAtSec == kEpoch2021_01_02);
    CHECK(out[1].UpdatedAtSec == kEpoch2021_01_02);
}

TEST_CASE("MapGitHubIssueComments — absent updated_at defaults UpdatedAtSec to CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 5;
    c["body"] = "no update field";
    c["created_at"] = "2021-01-01T00:00:00Z";
    // no updated_at
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == out[0].CreatedAtSec);
}

TEST_CASE("MapGitHubIssueComments — later updated_at yields UpdatedAtSec > CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 6;
    c["body"] = "edited later";
    c["created_at"] = "2021-01-01T00:00:00Z";
    c["updated_at"] = "2021-01-02T00:00:00Z";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_02);
    CHECK(out[0].UpdatedAtSec > out[0].CreatedAtSec);
}

TEST_CASE("MapGitHubIssueComments — missing user object yields empty Author") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 7;
    c["body"] = "anon";
    // no user
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author.empty());
    CHECK(out[0].Body == "anon");
}

TEST_CASE("MapGitHubIssueComments — user present but login missing yields empty Author") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 8;
    c["user"] = nlohmann::json::object(); // present but no login
    c["body"] = "x";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author.empty());
}

TEST_CASE("MapGitHubIssueComments — missing body yields empty Body") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 9;
    c["user"] = nlohmann::json::object();
    c["user"]["login"] = "octocat";
    // no body
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Body.empty());
    CHECK(out[0].Author == "octocat");
}

TEST_CASE("MapGitHubIssueComments — missing id yields empty Id") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    // no id
    c["body"] = "no id";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Id.empty());
}

TEST_CASE("MapGitHubIssueComments — non-integer id (string/float/null) yields empty Id") {
    SUBCASE("string id") {
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json c = nlohmann::json::object();
        c["id"] = "not-a-number";
        c["body"] = "x";
        arr.push_back(c);
        const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
        REQUIRE(out.size() == 1);
        CHECK(out[0].Id.empty());
    }
    SUBCASE("float id") {
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json c = nlohmann::json::object();
        c["id"] = 3.14;
        c["body"] = "x";
        arr.push_back(c);
        const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
        REQUIRE(out.size() == 1);
        CHECK(out[0].Id.empty());
    }
    SUBCASE("null id") {
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json c = nlohmann::json::object();
        c["id"] = nullptr;
        c["body"] = "x";
        arr.push_back(c);
        const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
        REQUIRE(out.size() == 1);
        CHECK(out[0].Id.empty());
    }
}

TEST_CASE("MapGitHubIssueComments — malformed timestamps leave epochs at 0 without throwing") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 10;
    c["body"] = "bad times";
    c["created_at"] = "not-a-date";
    c["updated_at"] = "also-garbage";
    arr.push_back(c);

    std::vector<TrackerIssueComment> out;
    CHECK_NOTHROW(out = MapGitHubIssueComments(arr));
    REQUIRE(out.size() == 1);
    // created_at unparseable → CreatedAtSec stays at its 0 default.
    CHECK(out[0].CreatedAtSec == 0);
    // updated_at unparseable → defaults to CreatedAtSec (== 0 here).
    CHECK(out[0].UpdatedAtSec == 0);
}

TEST_CASE("MapGitHubIssueComments — bad updated_at falls back to a valid CreatedAtSec") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 11;
    c["body"] = "good created, bad updated";
    c["created_at"] = "2021-01-01T00:00:00Z";
    c["updated_at"] = "garbage";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == kEpoch2021_01_01);
    CHECK(out[0].UpdatedAtSec == kEpoch2021_01_01);
}

TEST_CASE("MapGitHubIssueComments — empty timestamp strings leave epochs at 0") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json c = nlohmann::json::object();
    c["id"] = 12;
    c["body"] = "empty times";
    c["created_at"] = "";
    c["updated_at"] = "";
    arr.push_back(c);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 1);
    CHECK(out[0].CreatedAtSec == 0);
    CHECK(out[0].UpdatedAtSec == 0);
}

TEST_CASE("MapGitHubIssueComments — non-array input yields an empty vector (Pillar 3)") {
    SUBCASE("object") {
        nlohmann::json obj = nlohmann::json::object();
        obj["id"] = 1;
        CHECK(MapGitHubIssueComments(obj).empty());
    }
    SUBCASE("null") {
        nlohmann::json n = nlohmann::json();
        CHECK(MapGitHubIssueComments(n).empty());
    }
    SUBCASE("number") {
        nlohmann::json num = 42;
        CHECK(MapGitHubIssueComments(num).empty());
    }
    SUBCASE("string") {
        nlohmann::json s = "comments";
        CHECK(MapGitHubIssueComments(s).empty());
    }
}

TEST_CASE("MapGitHubIssueComments — empty array yields an empty vector") {
    nlohmann::json arr = nlohmann::json::array();
    CHECK(MapGitHubIssueComments(arr).empty());
}

TEST_CASE("MapGitHubIssueComments — non-object array elements are skipped, others mapped") {
    nlohmann::json arr = nlohmann::json::array();

    nlohmann::json good0 = nlohmann::json::object();
    good0["id"] = 100;
    good0["user"] = nlohmann::json::object();
    good0["user"]["login"] = "alice";
    good0["body"] = "real one";
    arr.push_back(good0);

    arr.push_back(nlohmann::json(7));               // number — skipped
    arr.push_back(nlohmann::json("scalar"));        // string — skipped
    arr.push_back(nlohmann::json::array());         // nested array — skipped
    arr.push_back(nlohmann::json());                // null — skipped

    nlohmann::json good1 = nlohmann::json::object();
    good1["id"] = 200;
    good1["user"] = nlohmann::json::object();
    good1["user"]["login"] = "bob";
    good1["body"] = "another real one";
    arr.push_back(good1);

    const std::vector<TrackerIssueComment> out = MapGitHubIssueComments(arr);
    REQUIRE(out.size() == 2);
    CHECK(out[0].Id == "100");
    CHECK(out[0].Author == "alice");
    CHECK(out[1].Id == "200");
    CHECK(out[1].Author == "bob");
}
