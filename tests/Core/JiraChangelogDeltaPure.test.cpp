#include <doctest/doctest.h>

// ticket-change-monitor plan, S1b — bucket-A coverage of the pure Jira changelog-delta
// parser: salient-field filtering against the resolved roster, display-preferred from->to +
// author extraction, self-author suppression, and the "changed since" minute-prefix window.
#include "JiraChangelogDeltaPure.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using JiraChangelogDeltaPure::DeltasFromIssueJson;
using JiraChangelogDeltaPure::JiraChangelogDelta;
using smatchet::SalientFieldRole;
using smatchet::TicketChangeKind;

namespace {

std::vector<SalientFieldRole> Roster() {
    std::vector<SalientFieldRole> roster;
    roster.push_back(SalientFieldRole{"status", "Status"});
    roster.push_back(SalientFieldRole{"assignee", "Assignee"});
    roster.push_back(SalientFieldRole{"priority", "Priority"});
    return roster;
}

} // namespace

TEST_CASE("DeltasFromIssueJson: salient filter + from->to + author") {
    const nlohmann::json issue = nlohmann::json::parse(R"json({
      "key": "PROJ-1",
      "changelog": { "histories": [
        {
          "id": "100",
          "created": "2026-06-20T08:00:00.000+0000",
          "author": { "accountId": "acc-bob", "displayName": "Bob" },
          "items": [
            { "fieldId": "status", "field": "status", "fromString": "To Do", "toString": "In Progress" },
            { "fieldId": "description", "field": "description", "fromString": "old", "toString": "new" }
          ]
        }
      ] }
    })json");

    const std::vector<JiraChangelogDelta> deltas = DeltasFromIssueJson(issue, "", Roster(), "");

    REQUIRE(deltas.size() == 1); // description item is non-salient -> dropped
    CHECK(deltas[0].summary.kind == TicketChangeKind::Modified);
    CHECK(deltas[0].summary.issueId == "PROJ-1");
    CHECK(deltas[0].summary.changedFieldLabel == "Status");
    CHECK(deltas[0].summary.fromValue == "To Do");
    CHECK(deltas[0].summary.toValue == "In Progress");
    CHECK(deltas[0].summary.author == "Bob");
    CHECK(deltas[0].changelogEntryId == "100");
    CHECK(deltas[0].createdAt == "2026-06-20T08:00:00.000+0000");
}

TEST_CASE("DeltasFromIssueJson: display-preferred, raw fallback, fieldId-less match") {
    SUBCASE("raw from/to used when display strings absent") {
        const nlohmann::json issue = nlohmann::json::parse(R"json({
          "key": "PROJ-2",
          "changelog": { "histories": [
            {
              "id": "7",
              "created": "2026-06-20T08:00:00.000+0000",
              "author": { "accountId": "acc-x", "displayName": "Xavier" },
              "items": [ { "fieldId": "priority", "field": "priority", "from": "3", "to": "1" } ]
            }
          ] }
        })json");

        const std::vector<JiraChangelogDelta> deltas = DeltasFromIssueJson(issue, "", Roster(), "");
        REQUIRE(deltas.size() == 1);
        CHECK(deltas[0].summary.changedFieldLabel == "Priority");
        CHECK(deltas[0].summary.fromValue == "3");
        CHECK(deltas[0].summary.toValue == "1");
    }

    SUBCASE("matches on `field` when `fieldId` is absent") {
        const nlohmann::json issue = nlohmann::json::parse(R"json({
          "key": "PROJ-3",
          "changelog": { "histories": [
            {
              "id": "8",
              "created": "2026-06-20T08:00:00.000+0000",
              "author": { "accountId": "acc-x", "displayName": "Xavier" },
              "items": [ { "field": "status", "fromString": "Open", "toString": "Done" } ]
            }
          ] }
        })json");

        const std::vector<JiraChangelogDelta> deltas = DeltasFromIssueJson(issue, "", Roster(), "");
        REQUIRE(deltas.size() == 1);
        CHECK(deltas[0].summary.changedFieldLabel == "Status");
        CHECK(deltas[0].summary.toValue == "Done");
    }
}

TEST_CASE("DeltasFromIssueJson: self-authored changes suppressed") {
    const nlohmann::json issue = nlohmann::json::parse(R"json({
      "key": "PROJ-4",
      "changelog": { "histories": [
        {
          "id": "9",
          "created": "2026-06-20T08:00:00.000+0000",
          "author": { "accountId": "acc-me", "displayName": "Me" },
          "items": [ { "fieldId": "status", "field": "status", "fromString": "A", "toString": "B" } ]
        }
      ] }
    })json");

    SUBCASE("self account id matches -> dropped") {
        const std::vector<JiraChangelogDelta> deltas = DeltasFromIssueJson(issue, "", Roster(), "acc-me");
        CHECK(deltas.empty());
    }
    SUBCASE("empty self account id -> kept") {
        const std::vector<JiraChangelogDelta> deltas = DeltasFromIssueJson(issue, "", Roster(), "");
        CHECK(deltas.size() == 1);
    }
}

TEST_CASE("DeltasFromIssueJson: minute-prefix since window") {
    const nlohmann::json issue = nlohmann::json::parse(R"json({
      "key": "PROJ-5",
      "changelog": { "histories": [
        {
          "id": "old",
          "created": "2026-06-20T07:50:00.000+0000",
          "author": { "accountId": "acc-x", "displayName": "Xavier" },
          "items": [ { "fieldId": "status", "field": "status", "fromString": "A", "toString": "B" } ]
        },
        {
          "id": "new",
          "created": "2026-06-20T08:05:00.000+0000",
          "author": { "accountId": "acc-x", "displayName": "Xavier" },
          "items": [ { "fieldId": "priority", "field": "priority", "fromString": "Low", "toString": "High" } ]
        }
      ] }
    })json");

    const std::vector<JiraChangelogDelta> deltas = DeltasFromIssueJson(issue, "2026-06-20T08:00", Roster(), "");
    REQUIRE(deltas.size() == 1); // only the 08:05 history is >= 08:00
    CHECK(deltas[0].changelogEntryId == "new");
    CHECK(deltas[0].summary.changedFieldLabel == "Priority");
}

TEST_CASE("DeltasFromIssueJson: malformed / missing input yields empty") {
    CHECK(DeltasFromIssueJson(nlohmann::json::array(), "", Roster(), "").empty());  // not an object
    CHECK(DeltasFromIssueJson(nlohmann::json::object(), "", Roster(), "").empty()); // no changelog
    const nlohmann::json noHist = nlohmann::json::parse(R"json({"key":"PROJ-6","changelog":{}})json");
    CHECK(DeltasFromIssueJson(noHist, "", Roster(), "").empty()); // changelog without histories
}
