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
using smatchet::plane::BuildPlaneSequenceIdInFilter;
using smatchet::plane::ExtractKeyFromPlaneQuery;
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

// --- Slice 0 WS2 of multi-grid-tabs — null / missing-relation / empty-optional
// mapping edges. Characterization only: these pin CURRENT behaviour ahead of the
// multi-pane refactor; a surprising result here is documented, not "fixed".

TEST_CASE("WS2 edge — explicit-null state_detail falls through to the state object") {
    nlohmann::json issue;
    issue["id"] = "uuid-ws2-1";
    issue["sequence_id"] = 11;
    issue["state_detail"] = nullptr; // present-but-null relation
    issue["state"] = {{"id", "state-from-base"}};

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {});
    CHECK(GetField(t, "status") == "state-from-base");
}

TEST_CASE("WS2 edge — all-null scalar optionals map to empty strings") {
    nlohmann::json issue;
    issue["id"] = "uuid-ws2-2";
    issue["sequence_id"] = 12;
    issue["priority"] = nullptr;
    issue["created_at"] = nullptr;
    issue["updated_at"] = nullptr;
    issue["description_stripped"] = nullptr;
    issue["labels"] = nullptr;
    issue["state"] = nullptr;

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {});
    CHECK(t.id == "SMT-12");
    CHECK(GetField(t, "priority").empty());
    CHECK(GetField(t, "created").empty());
    CHECK(GetField(t, "updated").empty());
    CHECK(GetField(t, "description").empty());
    CHECK(GetField(t, "labels").empty());
    CHECK(GetField(t, "status").empty());
}

TEST_CASE("WS2 edge — assignees array holding a null entry maps to an empty assignee") {
    nlohmann::json issue;
    issue["id"] = "uuid-ws2-3";
    issue["sequence_id"] = 13;
    issue["assignees"] = nlohmann::json::array({nullptr});

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {});
    CHECK(GetField(t, "assignee").empty());
}

TEST_CASE("WS2 edge — assignee_details entry without display_name keeps the raw id passthrough") {
    nlohmann::json issue;
    issue["id"] = "uuid-ws2-4";
    issue["sequence_id"] = 14;
    issue["assignees"] = nlohmann::json::array({{{"id", "user-77"}}});
    issue["assignee_details"] = nlohmann::json::array({{{"avatar", "x.png"}}}); // no display_name

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {});
    CHECK(GetField(t, "assignee") == "user-77");
}

TEST_CASE("WS2 edge — label_details entries missing both name and id leave separator artifacts (characterized)") {
    // CHARACTERIZATION: an empty label entry after a named one still appends the
    // ", " separator before its (empty) name — the joined string ends "x, ".
    // Pinned, not endorsed.
    nlohmann::json issue;
    issue["id"] = "uuid-ws2-5";
    issue["sequence_id"] = 15;
    issue["label_details"] = nlohmann::json::array({{{"name", "x"}}, nlohmann::json::object()});

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {});
    CHECK(GetField(t, "labels") == "x, ");
}

TEST_CASE("WS2 edge — explicit-null description_html stores no rich value") {
    nlohmann::json issue;
    issue["id"] = "uuid-ws2-6";
    issue["sequence_id"] = 16;
    issue["description_stripped"] = "plain";
    issue["description_html"] = nullptr;

    const CachedTicket t = MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {});
    CHECK(GetField(t, "description") == "plain");
    CHECK(GetRichField(t, "description").empty());
    CHECK(t.fieldRichValues.count("description") == 0);
}

TEST_CASE("WS2 edge — null entries inside the results array are skipped by the batch mapper") {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(nullptr);
    nlohmann::json good;
    good["id"] = "uuid-ws2-7";
    good["sequence_id"] = 17;
    results.push_back(good);

    std::unordered_map<std::string, std::string> keyToId;
    const auto tickets = MapPlaneWorkItemsArrayToCachedTickets(results, "SMT", {}, &keyToId);

    // The null entry maps to a ticket whose uuid + sequence are empty → id falls
    // back to the empty uuid → empty id → skipped by the batch guard.
    REQUIRE(tickets.size() == 1);
    CHECK(tickets[0].id == "SMT-17");
    CHECK(keyToId.count("SMT-17") == 1);
}

// user-info-window.md Slice 4 item 18b — `key` extraction from a Plane structured
// query (the single-issue narrowing hook for the activity window's "open issue").

TEST_CASE("ExtractKeyFromPlaneQuery — key member returned") {
    CHECK(ExtractKeyFromPlaneQuery(R"({"key":"SMT-7"})") == "SMT-7");
    CHECK(ExtractKeyFromPlaneQuery(R"({"project":"SMT","key":"SMT-7"})") == "SMT-7");
}

TEST_CASE("ExtractKeyFromPlaneQuery — project-only blob has no key clause") {
    CHECK(ExtractKeyFromPlaneQuery(R"({"project":"SMT"})").empty());
}

TEST_CASE("ExtractKeyFromPlaneQuery — empty / malformed / non-object input → empty") {
    CHECK(ExtractKeyFromPlaneQuery("").empty());
    CHECK(ExtractKeyFromPlaneQuery("{not json").empty());
    CHECK(ExtractKeyFromPlaneQuery(R"(["key"])").empty());
}

TEST_CASE("ExtractKeyFromPlaneQuery — non-string key member → empty") {
    CHECK(ExtractKeyFromPlaneQuery(R"({"key":42})").empty());
    CHECK(ExtractKeyFromPlaneQuery(R"({"key":null})").empty());
}

// security deep-audit target 11 — the Plane work-item mapper sits behind a
// possibly-malicious tracker or a MITM, so every shape of hostile JSON must map
// to a safe default rather than throw or over-read. nlohmann member access on a
// type-confused node throws type_error, so any missing guard would surface as an
// uncaught throw right here. The cases below lock the no-throw contract.

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — non-object top-level never throws" *
          doctest::test_suite("[high-risk]")) {
    CHECK_NOTHROW(MapPlaneWorkItemJsonToCachedTicket(nlohmann::json(nullptr), "SMT", {}));
    CHECK_NOTHROW(MapPlaneWorkItemJsonToCachedTicket(nlohmann::json::array(), "SMT", {}));
    CHECK_NOTHROW(MapPlaneWorkItemJsonToCachedTicket(nlohmann::json(42), "SMT", {}));
    CHECK_NOTHROW(MapPlaneWorkItemJsonToCachedTicket(nlohmann::json("scalar"), "SMT", {}));
    CHECK_NOTHROW(MapPlaneWorkItemJsonToCachedTicket(nlohmann::json(true), "SMT", {}));
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — every flat field type-confused never throws" *
          doctest::test_suite("[high-risk]")) {
    nlohmann::json issue;
    issue["id"] = 12345;                // number where a string is expected
    issue["sequence_id"] = "not-a-int"; // string where a number is expected
    issue["name"] = nlohmann::json::array({1, 2, 3});
    issue["description_stripped"] = false;
    issue["priority"] = nlohmann::json::object();
    issue["state_detail"] = "scalar-not-object";
    issue["assignee_details"] = 99;
    issue["label_details"] = "scalar-not-array";
    issue["cycle_details"] = nlohmann::json::array();
    issue["type_detail"] = true;
    issue["created_at"] = nlohmann::json::object();
    CHECK_NOTHROW(MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {}));
}

TEST_CASE("MapPlaneWorkItemJsonToCachedTicket — wrong-typed nested array entries never throw" *
          doctest::test_suite("[high-risk]")) {
    nlohmann::json issue;
    issue["id"] = "uuid-x";
    issue["sequence_id"] = 7;
    // assignee_details / label_details arrays whose ELEMENTS are the wrong type --
    // the mapper reads front only after an emptiness guard, then reads members.
    issue["assignee_details"] = nlohmann::json::array({42, "str", nullptr, true});
    issue["label_details"] = nlohmann::json::array({nlohmann::json::array(), 1, "x"});
    issue["assignees"] = nlohmann::json::array({nlohmann::json::object(), 5});
    issue["labels"] = nlohmann::json::array({true, false});
    CHECK_NOTHROW(MapPlaneWorkItemJsonToCachedTicket(issue, "SMT", {}));
}

TEST_CASE("MapPlaneWorkItemsArrayToCachedTickets — hostile entries skipped, valid row kept" *
          doctest::test_suite("[high-risk]")) {
    nlohmann::json results = nlohmann::json::array();
    results.push_back(nlohmann::json(nullptr));
    results.push_back(nlohmann::json(42));
    results.push_back("scalar");
    results.push_back(nlohmann::json::array());
    nlohmann::json broken;
    broken["state_detail"] = "scalar-not-object";
    broken["assignee_details"] = 7;
    results.push_back(broken);
    nlohmann::json good;
    good["id"] = "uuid-good";
    good["sequence_id"] = 9;
    good["name"] = "valid row";
    results.push_back(good);

    std::unordered_map<std::string, std::string> keyToId;
    std::vector<CachedTicket> tickets;
    CHECK_NOTHROW(tickets = MapPlaneWorkItemsArrayToCachedTickets(results, "SMT", {}, &keyToId));
    // Only the one well-formed row survives — every hostile entry maps to an
    // empty id and is dropped by the batch guard.
    REQUIRE(tickets.size() == 1);
    CHECK(tickets[0].id == "SMT-9");
    CHECK(keyToId.count("SMT-9") == 1);
}

// §B4-v2 — BuildPlaneSequenceIdInFilter: keys → comma-joined sequence_id CSV.
TEST_CASE("BuildPlaneSequenceIdInFilter: happy path maps visual keys to sequence ids") {
    CHECK(BuildPlaneSequenceIdInFilter({"SMT-1", "SMT-23", "SMT-456"}) == "1,23,456");
}

TEST_CASE("BuildPlaneSequenceIdInFilter: mixed project identifiers keep their own sequence ids") {
    // Only the numeric suffix matters; different identifiers with distinct sequences all contribute.
    CHECK(BuildPlaneSequenceIdInFilter({"ABC-7", "XYZ-8"}) == "7,8");
}

TEST_CASE("BuildPlaneSequenceIdInFilter: empty input yields empty filter") {
    CHECK(BuildPlaneSequenceIdInFilter({}).empty());
}

TEST_CASE("BuildPlaneSequenceIdInFilter: de-duplicates repeated sequence ids, first-seen order") {
    CHECK(BuildPlaneSequenceIdInFilter({"SMT-5", "SMT-5", "SMT-9", "SMT-5"}) == "5,9");
}

TEST_CASE("BuildPlaneSequenceIdInFilter: any unparseable key abandons the whole filter") {
    // A bare UUID (no '-<digits>' suffix) → empty, so the caller falls back to the full sweep
    // and never server-excludes an issue it still needs.
    CHECK(BuildPlaneSequenceIdInFilter({"SMT-1", "550e8400-e29b-41d4-a716-446655440000"}).empty());
    // Non-digit suffix (a UUID-tail after the last dash) → empty.
    CHECK(BuildPlaneSequenceIdInFilter({"SMT-abc"}).empty());
    // A dash with nothing after it → empty.
    CHECK(BuildPlaneSequenceIdInFilter({"SMT-"}).empty());
    // No dash at all → empty.
    CHECK(BuildPlaneSequenceIdInFilter({"SMT123"}).empty());
    // Trailing whitespace is not a digit → empty (keys are exact tokens, never trimmed here).
    CHECK(BuildPlaneSequenceIdInFilter({"SMT-12 "}).empty());
}

TEST_CASE("BuildPlaneSequenceIdInFilter: identifiers with internal dashes use the last segment") {
    // rfind('-') isolates the trailing sequence even when the project identifier contains a dash.
    CHECK(BuildPlaneSequenceIdInFilter({"MY-PROJ-42"}) == "42");
}
