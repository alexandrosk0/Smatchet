#include <doctest/doctest.h>

// user-info-window.md Slice 4 — bucket-A coverage of the pure GitHub activity-feed
// core (GitHubActivityFeedPure.cpp): /issues/events -> TrackerActivityEntry mapping
// (actor filter, day window, null-issue skip, label/assignee/rename details) and
// the newest-first page-loop early-stop rule.
#include "GitHubActivityFeed.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using GitHubActivityFeed::EntriesFromEventsJson;
using GitHubActivityFeed::PageEndsBeforeWindow;

namespace {

nlohmann::json MakeEvent(const std::string& actorLogin, const std::string& createdAt, int issueNumber) {
    nlohmann::json e;
    e["actor"] = nlohmann::json::object();
    e["actor"]["login"] = actorLogin;
    e["created_at"] = createdAt;
    e["event"] = "closed";
    e["issue"] = nlohmann::json::object();
    e["issue"]["number"] = issueNumber;
    e["issue"]["html_url"] = "https://github.com/o/r/issues/" + std::to_string(issueNumber);
    e["issue"]["title"] = "Issue " + std::to_string(issueNumber);
    return e;
}

} // namespace

TEST_CASE("EntriesFromEventsJson: happy path maps all entry fields") {
    nlohmann::json events = nlohmann::json::array();
    events.push_back(MakeEvent("octocat", "2026-06-03T10:00:00Z", 42));

    const auto entries = EntriesFromEventsJson(events, "octocat", "2026-06-01", "2026-06-07", "o", "r");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].Timestamp == "2026-06-03T10:00:00Z");
    CHECK(entries[0].IssueKey == "o/r#42");
    CHECK(entries[0].IssueUrl == "https://github.com/o/r/issues/42");
    CHECK(entries[0].Summary == "Issue 42");
    CHECK(entries[0].ActionLabel == "closed");
    CHECK(entries[0].Details.empty()); // closed carries no detail payload
}

TEST_CASE("EntriesFromEventsJson: other actors are skipped") {
    nlohmann::json events = nlohmann::json::array();
    events.push_back(MakeEvent("someone-else", "2026-06-03T10:00:00Z", 1));
    events.push_back(MakeEvent("octocat", "2026-06-03T11:00:00Z", 2));

    const auto entries = EntriesFromEventsJson(events, "octocat", "", "", "o", "r");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].IssueKey == "o/r#2");
}

TEST_CASE("EntriesFromEventsJson: day window clips both sides") {
    nlohmann::json events = nlohmann::json::array();
    events.push_back(MakeEvent("octocat", "2026-05-31T23:59:59Z", 1)); // before
    events.push_back(MakeEvent("octocat", "2026-06-01T00:00:00Z", 2)); // first day
    events.push_back(MakeEvent("octocat", "2026-06-07T12:00:00Z", 3)); // last day
    events.push_back(MakeEvent("octocat", "2026-06-08T00:00:01Z", 4)); // after

    const auto entries = EntriesFromEventsJson(events, "octocat", "2026-06-01", "2026-06-07", "o", "r");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].IssueKey == "o/r#2");
    CHECK(entries[1].IssueKey == "o/r#3");
}

TEST_CASE("EntriesFromEventsJson: events without usable issue context are skipped") {
    nlohmann::json noIssue = MakeEvent("octocat", "2026-06-03T10:00:00Z", 1);
    noIssue.erase("issue");
    nlohmann::json nullIssue = MakeEvent("octocat", "2026-06-03T10:00:00Z", 1);
    nullIssue["issue"] = nullptr;
    nlohmann::json zeroNumber = MakeEvent("octocat", "2026-06-03T10:00:00Z", 0);

    nlohmann::json events = nlohmann::json::array();
    events.push_back(noIssue);
    events.push_back(nullIssue);
    events.push_back(zeroNumber);

    CHECK(EntriesFromEventsJson(events, "octocat", "", "", "o", "r").empty());
}

TEST_CASE("EntriesFromEventsJson: detail payload variants") {
    nlohmann::json labeled = MakeEvent("octocat", "2026-06-03T10:00:00Z", 1);
    labeled["event"] = "labeled";
    labeled["label"] = nlohmann::json::object();
    labeled["label"]["name"] = "bug";

    nlohmann::json assigned = MakeEvent("octocat", "2026-06-03T11:00:00Z", 2);
    assigned["event"] = "assigned";
    assigned["assignee"] = nlohmann::json::object();
    assigned["assignee"]["login"] = "octocat";

    nlohmann::json renamed = MakeEvent("octocat", "2026-06-03T12:00:00Z", 3);
    renamed["event"] = "renamed";
    renamed["rename"] = nlohmann::json::object();
    renamed["rename"]["from"] = "Old title";
    renamed["rename"]["to"] = "New title";

    nlohmann::json renamedNoFrom = MakeEvent("octocat", "2026-06-03T13:00:00Z", 4);
    renamedNoFrom["event"] = "renamed";
    renamedNoFrom["rename"] = nlohmann::json::object();
    renamedNoFrom["rename"]["to"] = "Only new";

    nlohmann::json events = nlohmann::json::array();
    events.push_back(labeled);
    events.push_back(assigned);
    events.push_back(renamed);
    events.push_back(renamedNoFrom);

    const auto entries = EntriesFromEventsJson(events, "octocat", "", "", "o", "r");
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].Details == "bug");
    CHECK(entries[1].Details == "octocat");
    CHECK(entries[2].Details == "Old title -> New title");
    CHECK(entries[3].Details == "(none) -> Only new");
}

TEST_CASE("EntriesFromEventsJson: malformed payloads do not throw") {
    SUBCASE("non-array root") {
        CHECK(EntriesFromEventsJson(nlohmann::json::object(), "octocat", "", "", "o", "r").empty());
    }
    SUBCASE("null + non-object elements skipped") {
        nlohmann::json events = nlohmann::json::array();
        events.push_back(nullptr);
        events.push_back("string");
        events.push_back(7);
        CHECK(EntriesFromEventsJson(events, "octocat", "", "", "o", "r").empty());
    }
    SUBCASE("event without actor skipped") {
        nlohmann::json e = MakeEvent("octocat", "2026-06-03T10:00:00Z", 1);
        e.erase("actor");
        nlohmann::json events = nlohmann::json::array();
        events.push_back(e);
        CHECK(EntriesFromEventsJson(events, "octocat", "", "", "o", "r").empty());
    }
}

TEST_CASE("PageEndsBeforeWindow: newest-first early-stop rule") {
    nlohmann::json page = nlohmann::json::array();
    page.push_back(MakeEvent("octocat", "2026-06-05T10:00:00Z", 1));
    page.push_back(MakeEvent("octocat", "2026-05-30T10:00:00Z", 2)); // tail precedes window

    SUBCASE("tail before dayFrom stops") { CHECK(PageEndsBeforeWindow(page, "2026-06-01")); }
    SUBCASE("tail on dayFrom continues") { CHECK_FALSE(PageEndsBeforeWindow(page, "2026-05-30")); }
    SUBCASE("empty dayFrom never stops") { CHECK_FALSE(PageEndsBeforeWindow(page, "")); }
    SUBCASE("empty page never stops") { CHECK_FALSE(PageEndsBeforeWindow(nlohmann::json::array(), "2026-06-01")); }
    SUBCASE("non-array never stops") { CHECK_FALSE(PageEndsBeforeWindow(nlohmann::json::object(), "2026-06-01")); }
    SUBCASE("malformed tail never stops") {
        nlohmann::json badTail = nlohmann::json::array();
        badTail.push_back(MakeEvent("octocat", "2026-05-30T10:00:00Z", 1));
        badTail.push_back(nullptr);
        CHECK_FALSE(PageEndsBeforeWindow(badTail, "2026-06-01"));
    }
    SUBCASE("short tail timestamp never stops") {
        nlohmann::json shortTs = nlohmann::json::array();
        shortTs.push_back(MakeEvent("octocat", "bad", 1));
        CHECK_FALSE(PageEndsBeforeWindow(shortTs, "2026-06-01"));
    }
}
