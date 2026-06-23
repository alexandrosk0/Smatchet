// LinearIssueMappingPure doctest — Linear issue node -> CachedTicket.
// Slice 1/2 of docs/plans/linear-tracker-backend.md.

#include "LinearIssueMappingPure.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace smatchet::linear;

namespace {
nlohmann::json FullNode() {
    return nlohmann::json::parse(R"({
        "id": "uuid-1",
        "identifier": "ENG-123",
        "number": 123,
        "title": "Fix the thing",
        "description": "## details\nmarkdown body",
        "url": "https://linear.app/acme/issue/ENG-123",
        "priority": 2,
        "priorityLabel": "High",
        "createdAt": "2026-01-02T03:04:05.000Z",
        "updatedAt": "2026-02-03T04:05:06.000Z",
        "state": {"id": "s1", "name": "In Progress", "type": "started"},
        "assignee": {"id": "u1", "name": "jdoe", "displayName": "John Doe"},
        "creator": {"id": "u2", "name": "jane", "displayName": "Jane Roe"},
        "labels": {"nodes": [{"id":"l1","name":"bug"},{"id":"l2","name":"p1"}]},
        "team": {"id": "t1", "key": "ENG", "name": "Engineering"},
        "project": {"id": "p1", "name": "Q3 Roadmap"},
        "cycle": {"id": "c1", "number": 5, "name": "Sprint 5"}
    })");
}
} // namespace

TEST_CASE("MapLinearIssueNodeToCachedTicket — full node maps every field") {
    CachedTicket t = MapLinearIssueNodeToCachedTicket(FullNode());
    CHECK(t.id == "ENG-123");
    CHECK(t.GetFieldValue("summary") == "Fix the thing");
    CHECK(t.GetFieldValue("description") == "## details\nmarkdown body");
    CHECK(t.GetFieldValue("status") == "In Progress");
    CHECK(t.GetFieldValue("assignee") == "John Doe");
    CHECK(t.GetFieldValue("author") == "Jane Roe");
    CHECK(t.GetFieldValue("labels") == "bug, p1");
    CHECK(t.GetFieldValue("created") == "2026-01-02T03:04:05.000Z");
    CHECK(t.GetFieldValue("updated") == "2026-02-03T04:05:06.000Z");
    CHECK(t.GetFieldValue("priority") == "High");
    CHECK(t.GetFieldValue("project") == "Q3 Roadmap");
    CHECK(t.GetFieldValue("team") == "ENG");
    CHECK(t.GetFieldValue("cycle") == "Sprint 5");
    CHECK(t.GetFieldValue("url") == "https://linear.app/acme/issue/ENG-123");
}

TEST_CASE("MapLinearIssueNodeToCachedTicket — null/missing relations are safe + empty") {
    nlohmann::json node = nlohmann::json::parse(R"({
        "identifier": "ENG-9",
        "title": "Bare",
        "assignee": null,
        "project": null,
        "cycle": null,
        "labels": null,
        "state": null
    })");
    CachedTicket t = MapLinearIssueNodeToCachedTicket(node);
    CHECK(t.id == "ENG-9");
    CHECK(t.GetFieldValue("summary") == "Bare");
    CHECK(t.GetFieldValue("status").empty());
    CHECK(t.GetFieldValue("assignee").empty());
    CHECK(t.GetFieldValue("author").empty());
    CHECK(t.GetFieldValue("labels").empty());
    CHECK(t.GetFieldValue("project").empty());
    CHECK(t.GetFieldValue("cycle").empty());
    CHECK(t.GetFieldValue("priority").empty());
    CHECK(t.GetFieldValue("url").empty());
}

TEST_CASE("MapLinearIssueNodeToCachedTicket — priority + cycle fallbacks, displayName->name") {
    nlohmann::json node = nlohmann::json::parse(R"({
        "identifier": "ENG-1",
        "title": "t",
        "priority": 1,
        "assignee": {"name": "loginonly"},
        "cycle": {"number": 7}
    })");
    CachedTicket t = MapLinearIssueNodeToCachedTicket(node);
    CHECK(t.GetFieldValue("priority") == "Urgent");      // mapped from int when no label
    CHECK(t.GetFieldValue("assignee") == "loginonly");   // displayName absent -> name
    CHECK(t.GetFieldValue("cycle") == "Cycle 7");        // name absent -> "Cycle <number>"
}

TEST_CASE("MapLinearIssueNodeToCachedTicket — non-object is empty, never throws") {
    CachedTicket t = MapLinearIssueNodeToCachedTicket(nlohmann::json());
    CHECK(t.id.empty());
    CHECK(t.fieldValues["summary"].empty());
}

TEST_CASE("MapLinearIssueNodesToTickets — array maps in order, skips non-objects") {
    nlohmann::json nodes = nlohmann::json::parse(R"([
        {"identifier":"ENG-1","title":"a"},
        "not-an-object",
        {"identifier":"ENG-2","title":"b"}
    ])");
    std::vector<CachedTicket> ts = MapLinearIssueNodesToTickets(nodes);
    REQUIRE(ts.size() == 2);
    CHECK(ts[0].id == "ENG-1");
    CHECK(ts[1].id == "ENG-2");

    CHECK(MapLinearIssueNodesToTickets(nlohmann::json::object()).empty()); // non-array -> empty
}
