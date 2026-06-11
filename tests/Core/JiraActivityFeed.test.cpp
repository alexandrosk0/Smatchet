#include <doctest/doctest.h>

// user-info-window.md Slice 3 — bucket-A coverage of the pure activity-feed core:
// discovery-JQL build, estimate formatting, changelog-JSON -> TrackerActivityEntry
// mapping (author filter + day window + estimate display), and the two free-text
// scan helpers the User Info window's "add to query" affordance uses.
#include "P4AnnotateParse.h"
#include "Tracker/ProjectResolver.h"

#include "JiraActivityFeed.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using JiraActivityFeed::BuildActivityDiscoveryJql;
using JiraActivityFeed::EntriesFromIssueJson;
using JiraActivityFeed::FormatEstimateSeconds;

TEST_CASE("BuildActivityDiscoveryJql: clause assembly") {
    SUBCASE("all clauses present") {
        const std::string jql = BuildActivityDiscoveryJql("acc-1", "2026-06-01", "2026-06-07", "PROJ");
        CHECK(jql == "(assignee WAS \"acc-1\" OR reporter WAS \"acc-1\")"
                     " AND updated >= \"2026-06-01\" AND updated <= \"2026-06-07\""
                     " AND project = \"PROJ\" ORDER BY updated DESC");
    }

    SUBCASE("empty bounds + scope drop their clauses") {
        const std::string jql = BuildActivityDiscoveryJql("acc-1", "", "", "");
        CHECK(jql == "(assignee WAS \"acc-1\" OR reporter WAS \"acc-1\") ORDER BY updated DESC");
    }

    SUBCASE("only dayFrom") {
        const std::string jql = BuildActivityDiscoveryJql("acc-1", "2026-06-01", "", "");
        CHECK(jql == "(assignee WAS \"acc-1\" OR reporter WAS \"acc-1\")"
                     " AND updated >= \"2026-06-01\" ORDER BY updated DESC");
    }

    SUBCASE("quotes and backslashes in inputs are JQL-escaped") {
        const std::string jql = BuildActivityDiscoveryJql("a\"b\\c", "", "", "P\"Q");
        CHECK(jql.find("\"a\\\"b\\\\c\"") != std::string::npos);
        CHECK(jql.find("project = \"P\\\"Q\"") != std::string::npos);
    }
}

TEST_CASE("FormatEstimateSeconds: 8h-workday formatting + passthrough") {
    SUBCASE("zero is 0m") { CHECK(FormatEstimateSeconds("0") == "0m"); }
    SUBCASE("minutes only") { CHECK(FormatEstimateSeconds("1800") == "30m"); }
    SUBCASE("hours only") { CHECK(FormatEstimateSeconds("7200") == "2h"); }
    SUBCASE("exactly one workday") { CHECK(FormatEstimateSeconds("28800") == "1d"); }
    SUBCASE("mixed units") {
        // 1d (28800) + 2h (7200) + 30m (1800) = 37800
        CHECK(FormatEstimateSeconds("37800") == "1d 2h 30m");
    }
    SUBCASE("sub-minute remainder truncates") { CHECK(FormatEstimateSeconds("59") == "0m"); }
    SUBCASE("empty passthrough") { CHECK(FormatEstimateSeconds("") == ""); }
    SUBCASE("non-digit passthrough") { CHECK(FormatEstimateSeconds("2 days") == "2 days"); }
    SUBCASE("negative is non-digit passthrough") { CHECK(FormatEstimateSeconds("-60") == "-60"); }
    SUBCASE("overflow passthrough") {
        const std::string huge(25, '9'); // > LLONG_MAX digits
        CHECK(FormatEstimateSeconds(huge) == huge);
    }
}

namespace {

/// Minimal issue JSON with one changelog history (author `acc`, timestamp `created`)
/// carrying one item. Tests mutate the returned object for their specific shapes.
nlohmann::json MakeIssue(const std::string& acc, const std::string& created) {
    nlohmann::json item = {
        {"field", "status"}, {"from", "1"}, {"fromString", "To Do"}, {"to", "2"}, {"toString", "In Progress"}};
    nlohmann::json history = {{"author", {{"accountId", acc}}}, {"created", created}, {"items", {item}}};
    return nlohmann::json{
        {"key", "PROJ-7"}, {"fields", {{"summary", "Fix the widget"}}}, {"changelog", {{"histories", {history}}}}};
}

} // namespace

TEST_CASE("EntriesFromIssueJson: changelog -> entries") {
    SUBCASE("happy path maps every field") {
        const auto entries = EntriesFromIssueJson(MakeIssue("acc-1", "2026-06-03T10:00:00.000+0000"), "acc-1",
                                                  "2026-06-01", "2026-06-07", "https://x.atlassian.net");
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].Timestamp == "2026-06-03T10:00:00.000+0000");
        CHECK(entries[0].IssueKey == "PROJ-7");
        CHECK(entries[0].IssueUrl == "https://x.atlassian.net/browse/PROJ-7");
        CHECK(entries[0].Summary == "Fix the widget");
        CHECK(entries[0].ActionLabel == "status");
        CHECK(entries[0].Details == "To Do -> In Progress");
    }

    SUBCASE("other authors' histories are skipped") {
        const auto entries = EntriesFromIssueJson(MakeIssue("acc-OTHER", "2026-06-03T10:00:00.000+0000"), "acc-1", "",
                                                  "", "https://x.atlassian.net");
        CHECK(entries.empty());
    }

    SUBCASE("day window excludes out-of-range timestamps") {
        const auto issue = MakeIssue("acc-1", "2026-05-20T10:00:00.000+0000");
        CHECK(EntriesFromIssueJson(issue, "acc-1", "2026-06-01", "2026-06-07", "").empty());
        CHECK(EntriesFromIssueJson(issue, "acc-1", "", "2026-05-19", "").empty());
        CHECK(EntriesFromIssueJson(issue, "acc-1", "2026-05-20", "2026-05-20", "").size() == 1);
    }

    SUBCASE("short timestamp is kept unconditionally") {
        const auto issue = MakeIssue("acc-1", "bad-ts");
        CHECK(EntriesFromIssueJson(issue, "acc-1", "2026-06-01", "2026-06-07", "").size() == 1);
    }

    SUBCASE("empty bound is open on that side") {
        const auto issue = MakeIssue("acc-1", "2099-01-01T00:00:00.000+0000");
        CHECK(EntriesFromIssueJson(issue, "acc-1", "2026-06-01", "", "").size() == 1);
    }

    SUBCASE("estimate fields format raw seconds when no display string") {
        auto issue = MakeIssue("acc-1", "2026-06-03T10:00:00.000+0000");
        issue["changelog"]["histories"][0]["items"][0] = {
            {"field", "timeoriginalestimate"}, {"from", ""}, {"fromString", ""}, {"to", "37800"}, {"toString", ""}};
        const auto entries = EntriesFromIssueJson(issue, "acc-1", "", "", "");
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].Details == "(none) -> 1d 2h 30m");
    }

    SUBCASE("display string wins over raw value") {
        auto issue = MakeIssue("acc-1", "2026-06-03T10:00:00.000+0000");
        issue["changelog"]["histories"][0]["items"][0]["toString"] = "Done";
        const auto entries = EntriesFromIssueJson(issue, "acc-1", "", "", "");
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].Details == "To Do -> Done");
    }

    SUBCASE("empty browseBase or key yields empty IssueUrl") {
        const auto entries =
            EntriesFromIssueJson(MakeIssue("acc-1", "2026-06-03T10:00:00.000+0000"), "acc-1", "", "", "");
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].IssueUrl == "");
    }

    SUBCASE("malformed shapes return empty without throwing") {
        CHECK(EntriesFromIssueJson(nlohmann::json::array(), "acc-1", "", "", "").empty());
        CHECK(EntriesFromIssueJson(nlohmann::json{{"key", "K-1"}}, "acc-1", "", "", "").empty());
        nlohmann::json badHistories = {{"key", "K-1"}, {"changelog", {{"histories", "not-an-array"}}}};
        CHECK(EntriesFromIssueJson(badHistories, "acc-1", "", "", "").empty());
    }

    SUBCASE("multiple items in one history each yield an entry") {
        auto issue = MakeIssue("acc-1", "2026-06-03T10:00:00.000+0000");
        issue["changelog"]["histories"][0]["items"].push_back(
            {{"field", "assignee"}, {"from", ""}, {"fromString", ""}, {"to", ""}, {"toString", "Alice"}});
        const auto entries = EntriesFromIssueJson(issue, "acc-1", "", "", "");
        REQUIRE(entries.size() == 2);
        CHECK(entries[1].ActionLabel == "assignee");
        CHECK(entries[1].Details == "(none) -> Alice");
    }
}

TEST_CASE("FindFirstChangelistInText: CL scan for the activity 'add to query' affordance") {
    using P4AnnotateParse::FindFirstChangelistInText;

    SUBCASE("finds a 6+ digit run with offsets") {
        const auto m = FindFirstChangelistInText("submitted in 1234567 yesterday");
        CHECK(m.Cl == "1234567");
        CHECK(m.Start == 13);
        CHECK(m.End == 20);
    }

    SUBCASE("five digits is not a CL") {
        const auto m = FindFirstChangelistInText("see 12345 there");
        CHECK(m.Cl == "");
        CHECK(m.Start == m.End);
    }

    SUBCASE("issue-key digit tail is not misread as a CL") {
        const auto m = FindFirstChangelistInText("PROJ-123456 regressed");
        CHECK(m.Cl == "");
    }

    SUBCASE("CL after an issue key is still found") {
        const auto m = FindFirstChangelistInText("PROJ-123456 fixed by 987654");
        CHECK(m.Cl == "987654");
    }
}

TEST_CASE("FindFirstIssueKeyInText: issue-key scan for the activity 'add to query' affordance") {
    using smatchet::FindFirstIssueKeyInText;

    SUBCASE("finds a key with offsets") {
        const auto m = FindFirstIssueKeyInText("see PROJ-123 for details");
        CHECK(m.Key == "PROJ-123");
        CHECK(m.Start == 4);
        CHECK(m.End == 12);
    }

    SUBCASE("no key returns empty with Start == End") {
        const auto m = FindFirstIssueKeyInText("nothing to see here 404");
        CHECK(m.Key == "");
        CHECK(m.Start == m.End);
    }

    SUBCASE("UUID-style token is not a key") {
        const auto m = FindFirstIssueKeyInText("id 550e8400-e29b-41d4-a716-446655440000 logged");
        CHECK(m.Key == "");
    }
}
