// Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — pure-logic doctest
// for the Plane JSON → CachedTicket mapper. Mirrors GitHubIssueSearchMapping.test.cpp
// in shape. No HTTP, no PlaneClient instance.

#include "PlaneIssueMappingPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

using smatchet::plane::BuildPlaneAuthHeaders;
using smatchet::plane::MapPlaneWorkItemJsonToCachedTicket;
using smatchet::plane::MapPlaneWorkItemsArrayToCachedTickets;
using smatchet::plane::NextPaginationCursor;
using smatchet::plane::UserDisplayLookup;

namespace {

std::string GetField(const CachedTicket& t, const std::string& key) {
    const auto it = t.fieldValues.find(key);
    return it == t.fieldValues.end() ? std::string() : it->second;
}

std::string GetRichField(const CachedTicket& t, const std::string& key) {
    const auto it = t.fieldRichValues.find(key);
    return it == t.fieldRichValues.end() ? std::string() : it->second;
}

} // namespace

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — visual key uses identifier-sequence when both present") {
    nlohmann::json issue;
    issue["id"] = "uuid-1";
    issue["sequence_id"] = 42;
    issue["name"] = "  My issue  ";
    issue["state_detail"] = {{"id", "state-todo"}, {"name", "Todo"}};
    issue["priority"] = "high";
    issue["created_at"] = "2026-05-20T10:00:00Z";
    issue["updated_at"] = "2026-05-21T10:00:00Z";

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {});

    CHECK(t.id == "SMT-42");
    CHECK(GetField(t, "key") == "SMT-42");
    CHECK(GetField(t, "uuid") == "uuid-1");
    CHECK(GetField(t, "summary") == "My issue"); // trimmed
    CHECK(GetField(t, "status") == "state-todo");
    CHECK(GetField(t, "priority") == "high");
    CHECK(GetField(t, "created") == "2026-05-20T10:00:00Z");
    CHECK(GetField(t, "updated") == "2026-05-21T10:00:00Z");
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — visual key falls back to #seq then uuid") {
    nlohmann::json issue;
    issue["id"] = "uuid-x";
    issue["sequence_id"] = 7;
    issue["name"] = "no-identifier";

    const CachedTicket noIdent = MapPlaneWorkItemJsonToCachedTicket(issue, std::string(), {});
    CHECK(noIdent.id == "#7");

    nlohmann::json issue2;
    issue2["id"] = "uuid-only";
    issue2["name"] = "only-uuid";
    const CachedTicket onlyUuid = MapPlaneWorkItemJsonToCachedTicket(issue2, "SMT", {});
    CHECK(onlyUuid.id == "uuid-only");
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — state nesting variants all map to status") {
    // state_detail.id wins.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["state_detail"] = {{"id", "S1"}};
        issue["state"] = {{"id", "S2"}};
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "status") == "S1");
    }
    // state.id wins when state_detail missing.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["state"] = {{"id", "S2"}};
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "status") == "S2");
    }
    // flat string fallback.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["state"] = "S-flat";
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "status") == "S-flat");
    }
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — sprint (cycle) nesting variants all map to sprint") {
    // cycle_details.id wins over cycle.id.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["cycle_details"] = {{"id", "C1"}};
        issue["cycle"] = {{"id", "C2"}};
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "sprint") == "C1");
    }
    // cycle.id wins when cycle_details missing.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["cycle"] = {{"id", "C2"}};
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "sprint") == "C2");
    }
    // flat string fallback.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["cycle"] = "C-flat";
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "sprint") == "C-flat");
    }
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — issuetype nesting variants use the name inner-key") {
    // type_detail.name wins.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["type_detail"] = {{"id", "T1"}, {"name", "Bug"}};
        issue["type"] = {{"name", "Story"}};
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "issuetype") == "Bug");
    }
    // type.name wins when type_detail missing.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["type"] = {{"name", "Story"}};
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "issuetype") == "Story");
    }
    // flat string fallback.
    {
        nlohmann::json issue;
        issue["id"] = "u";
        issue["sequence_id"] = 1;
        issue["type"] = "Task";
        const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
        CHECK(GetField(t, "issuetype") == "Task");
    }
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — assignee_details display_name preferred over user lookup") {
    nlohmann::json issue;
    issue["id"] = "u";
    issue["sequence_id"] = 1;
    issue["assignees"] = nlohmann::json::array({{{"id", "acct-alice"}}});
    issue["assignee_details"] = nlohmann::json::array({{{"display_name", "Alice From Details"}}});

    // Even though the lookup vector has a different display, assignee_details wins.
    const std::vector<UserDisplayLookup> users = {{"acct-alice", "Alice Lookup"}};
    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", users);
    CHECK(GetField(t, "assignee") == "Alice From Details");
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — assignee id passthrough when no details and no lookup hit") {
    nlohmann::json issue;
    issue["id"] = "u";
    issue["sequence_id"] = 1;
    issue["assignees"] = nlohmann::json::array({"acct-unknown"});

    const std::vector<UserDisplayLookup> users = {{"acct-other", "Other"}};
    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", users);
    CHECK(GetField(t, "assignee") == "acct-unknown");
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — labels join with comma; label_details preferred") {
    nlohmann::json issue;
    issue["id"] = "u";
    issue["sequence_id"] = 1;
    issue["label_details"] =
        nlohmann::json::array({{{"id", "L1"}, {"name", "bug"}}, {{"id", "L2"}, {"name", "critical"}}});
    issue["labels"] = nlohmann::json::array({{{"id", "X"}, {"name", "ignored"}}});

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
    CHECK(GetField(t, "labels") == "bug, critical");
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — labels fallback to labels array (object + string)") {
    nlohmann::json issue;
    issue["id"] = "u";
    issue["sequence_id"] = 1;
    issue["labels"] = nlohmann::json::array({{{"name", "enhancement"}}, "raw-string"});

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
    CHECK(GetField(t, "labels") == "enhancement, raw-string");
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — assignee display resolved from user lookup") {
    nlohmann::json issue;
    issue["id"] = "u";
    issue["sequence_id"] = 1;
    issue["assignees"] = nlohmann::json::array({"acct-alice"});

    const std::vector<UserDisplayLookup> users = {{"acct-alice", "Alice T."}, {"acct-bob", "Bob R."}};
    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", users);
    CHECK(GetField(t, "assignee") == "Alice T.");
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — description_html preserved in fieldRichValues") {
    nlohmann::json issue;
    issue["id"] = "u";
    issue["sequence_id"] = 1;
    issue["description_stripped"] = "Hello world";
    issue["description_html"] = "<p>Hello <em>world</em></p>";

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "P", {});
    CHECK(GetField(t, "description") == "Hello world");
    CHECK(GetRichField(t, "description") == "<p>Hello <em>world</em></p>");
}

TEST_CASE("MapPlaneWorkItemsArrayToCachedTickets — fills keyToId; skips empty-id rows") {
    nlohmann::json results = nlohmann::json::array();
    {
        nlohmann::json a;
        a["id"] = "uuid-A";
        a["sequence_id"] = 1;
        a["name"] = "A";
        results.push_back(a);
    }
    {
        nlohmann::json b;
        b["id"] = "uuid-B";
        b["sequence_id"] = 2;
        b["name"] = "B";
        results.push_back(b);
    }

    std::unordered_map<std::string, std::string> keyToId;
    const std::vector<CachedTicket> tickets = MapPlaneWorkItemsArrayToCachedTickets(results, "SMT", {}, &keyToId);

    REQUIRE(tickets.size() == 2);
    CHECK(tickets[0].id == "SMT-1");
    CHECK(tickets[1].id == "SMT-2");
    CHECK(keyToId.at("SMT-1") == "uuid-A");
    CHECK(keyToId.at("SMT-2") == "uuid-B");
}

TEST_CASE("MapPlaneWorkItemsArrayToCachedTickets — non-array input returns empty vector") {
    nlohmann::json notArray = nlohmann::json::object();
    const std::vector<CachedTicket> tickets = MapPlaneWorkItemsArrayToCachedTickets(notArray, "SMT", {}, nullptr);
    CHECK(tickets.empty());
}

TEST_CASE("BuildPlaneAuthHeaders — exact three-header shape with x-api-key") {
    const auto h = BuildPlaneAuthHeaders("secret-key-123");
    REQUIRE(h.size() == 3);
    CHECK(h.at("Accept") == "application/json");
    CHECK(h.at("Content-Type") == "application/json");
    CHECK(h.at("x-api-key") == "secret-key-123");
}

TEST_CASE("NextPaginationCursor — pagination boundary cases") {
    // No `next_page_results` → empty cursor (no more pages).
    {
        nlohmann::json p;
        p["next_cursor"] = "tok";
        CHECK(NextPaginationCursor(p).empty());
    }
    // `next_page_results = false` → empty cursor.
    {
        nlohmann::json p;
        p["next_page_results"] = false;
        p["next_cursor"] = "tok";
        CHECK(NextPaginationCursor(p).empty());
    }
    // `next_page_results = true` + cursor present → returns cursor.
    {
        nlohmann::json p;
        p["next_page_results"] = true;
        p["next_cursor"] = "tok-abc";
        CHECK(NextPaginationCursor(p) == "tok-abc");
    }
    // `next_page_results = true` but cursor missing → empty.
    {
        nlohmann::json p;
        p["next_page_results"] = true;
        CHECK(NextPaginationCursor(p).empty());
    }
    // Non-object payload → empty.
    {
        nlohmann::json p = nlohmann::json::array();
        CHECK(NextPaginationCursor(p).empty());
    }
}
