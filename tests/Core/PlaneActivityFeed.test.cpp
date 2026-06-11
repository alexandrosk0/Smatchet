#include <doctest/doctest.h>

// user-info-window.md Slice 4 — bucket-A coverage of the pure Plane activity-feed
// core (PlaneActivityFeedPure.cpp): /activities/ results -> TrackerActivityEntry
// mapping (actor filter, string-vs-object actor, day window, label fallback chain,
// "(none)" detail placeholders) and the discovery-scan skip rule.
#include "PlaneActivityFeed.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using PlaneActivityFeed::EntriesFromActivitiesJson;
using PlaneActivityFeed::WorkItemWorthScanning;

namespace {

nlohmann::json MakeActivity(const std::string& actorId, const std::string& createdAt) {
    nlohmann::json a;
    a["actor"] = actorId;
    a["created_at"] = createdAt;
    a["field"] = "state";
    a["old_value"] = "Todo";
    a["new_value"] = "Done";
    return a;
}

} // namespace

TEST_CASE("EntriesFromActivitiesJson: happy path maps all entry fields") {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(MakeActivity("acc-1", "2026-06-03T10:00:00Z"));

    const auto entries = EntriesFromActivitiesJson(results, "acc-1", "2026-06-01", "2026-06-07", "SMT-7", "Fix crash",
                                                   "https://x/SMT-7");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].Timestamp == "2026-06-03T10:00:00Z");
    CHECK(entries[0].IssueKey == "SMT-7");
    CHECK(entries[0].IssueUrl == "https://x/SMT-7");
    CHECK(entries[0].Summary == "Fix crash");
    CHECK(entries[0].ActionLabel == "state");
    CHECK(entries[0].Details == "Todo -> Done");
}

TEST_CASE("EntriesFromActivitiesJson: other actors are skipped") {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(MakeActivity("acc-other", "2026-06-03T10:00:00Z"));
    results.push_back(MakeActivity("acc-1", "2026-06-03T11:00:00Z"));

    const auto entries = EntriesFromActivitiesJson(results, "acc-1", "", "", "SMT-7", "s", "u");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].Timestamp == "2026-06-03T11:00:00Z");
}

TEST_CASE("EntriesFromActivitiesJson: actor accepted as object with id member") {
    nlohmann::json a = MakeActivity("", "2026-06-03T10:00:00Z");
    a["actor"] = nlohmann::json::object();
    a["actor"]["id"] = "acc-1";
    nlohmann::json results = nlohmann::json::array();
    results.push_back(a);

    const auto entries = EntriesFromActivitiesJson(results, "acc-1", "", "", "SMT-7", "s", "u");
    CHECK(entries.size() == 1);
}

TEST_CASE("EntriesFromActivitiesJson: day window") {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(MakeActivity("acc-1", "2026-05-31T23:59:59Z")); // before window
    results.push_back(MakeActivity("acc-1", "2026-06-01T00:00:00Z")); // first day
    results.push_back(MakeActivity("acc-1", "2026-06-07T12:00:00Z")); // last day
    results.push_back(MakeActivity("acc-1", "2026-06-08T00:00:01Z")); // after window

    SUBCASE("both bounds clip") {
        const auto entries = EntriesFromActivitiesJson(results, "acc-1", "2026-06-01", "2026-06-07", "K", "s", "u");
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].Timestamp == "2026-06-01T00:00:00Z");
        CHECK(entries[1].Timestamp == "2026-06-07T12:00:00Z");
    }

    SUBCASE("single-day window") {
        const auto entries = EntriesFromActivitiesJson(results, "acc-1", "2026-06-07", "2026-06-07", "K", "s", "u");
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].Timestamp == "2026-06-07T12:00:00Z");
    }

    SUBCASE("open lower bound keeps everything up to dayTo") {
        const auto entries = EntriesFromActivitiesJson(results, "acc-1", "", "2026-06-01", "K", "s", "u");
        CHECK(entries.size() == 2);
    }

    SUBCASE("open upper bound keeps everything from dayFrom") {
        const auto entries = EntriesFromActivitiesJson(results, "acc-1", "2026-06-08", "", "K", "s", "u");
        CHECK(entries.size() == 1);
    }

    SUBCASE("short timestamp is kept unconditionally") {
        nlohmann::json shortTs = nlohmann::json::array();
        shortTs.push_back(MakeActivity("acc-1", "bad"));
        const auto entries = EntriesFromActivitiesJson(shortTs, "acc-1", "2026-06-01", "2026-06-07", "K", "s", "u");
        CHECK(entries.size() == 1);
    }
}

TEST_CASE("EntriesFromActivitiesJson: label falls back field -> verb -> updated") {
    nlohmann::json results = nlohmann::json::array();

    nlohmann::json verbOnly = MakeActivity("acc-1", "2026-06-03T10:00:00Z");
    verbOnly.erase("field");
    verbOnly["verb"] = "created";
    results.push_back(verbOnly);

    nlohmann::json neither = MakeActivity("acc-1", "2026-06-03T11:00:00Z");
    neither.erase("field");
    results.push_back(neither);

    const auto entries = EntriesFromActivitiesJson(results, "acc-1", "", "", "K", "s", "u");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].ActionLabel == "created");
    CHECK(entries[1].ActionLabel == "updated");
}

TEST_CASE("EntriesFromActivitiesJson: missing values render as (none) placeholders") {
    nlohmann::json a = MakeActivity("acc-1", "2026-06-03T10:00:00Z");
    a.erase("old_value");
    a.erase("new_value");
    nlohmann::json results = nlohmann::json::array();
    results.push_back(a);

    const auto entries = EntriesFromActivitiesJson(results, "acc-1", "", "", "K", "s", "u");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].Details == "(none) -> (none)");
}

TEST_CASE("EntriesFromActivitiesJson: malformed payloads do not throw") {
    SUBCASE("non-array root") {
        CHECK(EntriesFromActivitiesJson(nlohmann::json::object(), "acc-1", "", "", "K", "s", "u").empty());
    }
    SUBCASE("null + non-object elements skipped") {
        nlohmann::json results = nlohmann::json::array();
        results.push_back(nullptr);
        results.push_back("just a string");
        results.push_back(42);
        CHECK(EntriesFromActivitiesJson(results, "acc-1", "", "", "K", "s", "u").empty());
    }
    SUBCASE("activity without actor skipped") {
        nlohmann::json a;
        a["created_at"] = "2026-06-03T10:00:00Z";
        nlohmann::json results = nlohmann::json::array();
        results.push_back(a);
        CHECK(EntriesFromActivitiesJson(results, "acc-1", "", "", "K", "s", "u").empty());
    }
}

TEST_CASE("WorkItemWorthScanning: discovery skip rule") {
    CHECK(WorkItemWorthScanning("2026-06-05T10:00:00Z", "2026-06-01"));       // in window
    CHECK(WorkItemWorthScanning("2026-06-01T00:00:00Z", "2026-06-01"));       // boundary day scans
    CHECK_FALSE(WorkItemWorthScanning("2026-05-31T23:59:59Z", "2026-06-01")); // stale item skipped
    CHECK(WorkItemWorthScanning("2026-05-31T23:59:59Z", ""));                 // open lower bound scans
    CHECK(WorkItemWorthScanning("bad", "2026-06-01"));                        // short timestamp scans
    CHECK(WorkItemWorthScanning("", "2026-06-01"));                           // missing timestamp scans
}
