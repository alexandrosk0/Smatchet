// Slice 1 of deterministic-jira-test-backend — pure-logic doctest
// for the Jira JSON → CachedTicket mapper and field-list builder. No HTTP, no cpr,
// no JiraClient instance. Mirrors PlaneIssueMappingPure.test.cpp in shape.

#include "JiraIssueMappingPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// ViewsStore is needed to exercise BuildFetchFieldListsFromView.
#include "ConfigManager.h"

using smatchet::jira::AppendCachedTicketFromJiraSearchIssue;
using smatchet::jira::BuildFetchFieldListsFromView;

namespace {

std::string GetField(const CachedTicket& t, const std::string& key) {
    const auto it = t.fieldValues.find(key);
    return it == t.fieldValues.end() ? std::string() : it->second;
}

std::string GetRichField(const CachedTicket& t, const std::string& key) {
    const auto it = t.fieldRichValues.find(key);
    return it == t.fieldRichValues.end() ? std::string() : it->second;
}

nlohmann::json MakeIssue(const std::string& key, nlohmann::json fields) {
    nlohmann::json issue;
    issue["key"] = key;
    issue["fields"] = std::move(fields);
    return issue;
}

auto NoCommentFetch() {
    return [](const std::string&, nlohmann::json&) -> bool { return false; };
}

} // namespace

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — basic key and selected field mapping") {
    nlohmann::json fields;
    fields["summary"] = "Fix the bug";
    fields["status"] = {{"name", "In Progress"}};
    fields["priority"] = {{"name", "High"}};
    fields["assignee"] = {{"displayName", "Alice"}};

    const nlohmann::json issue = MakeIssue("SMAT-1", fields);
    const std::vector<std::string> selected = {"summary", "status", "priority", "assignee"};

    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == "SMAT-1");
    CHECK(GetField(results[0], "summary") == "Fix the bug");
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — missing fields object does not throw") {
    nlohmann::json issue;
    issue["key"] = "SMAT-2";
    // deliberately no "fields" key

    const std::vector<std::string> selected = {"summary", "status"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == "SMAT-2");
    CHECK(GetField(results[0], "summary").empty());
    CHECK(GetField(results[0], "status").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — changelog mapped to history field") {
    nlohmann::json fields;
    fields["summary"] = "History test";

    nlohmann::json issue = MakeIssue("SMAT-3", fields);
    issue["changelog"]["histories"] = nlohmann::json::array();

    const std::vector<std::string> selected = {"summary", "history"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].fieldValues.count("history") == 1);
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — watchers falls back to watches alias") {
    nlohmann::json fields;
    fields["watches"] = {{"watchCount", 3}};

    const nlohmann::json issue = MakeIssue("SMAT-4", fields);
    const std::vector<std::string> selected = {"watchers"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(!GetField(results[0], "watchers").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — watchers absent yields empty string") {
    nlohmann::json fields;
    fields["summary"] = "No watchers";

    const nlohmann::json issue = MakeIssue("SMAT-5", fields);
    const std::vector<std::string> selected = {"watchers"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(GetField(results[0], "watchers").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — comment mapped even when NOT a selected field "
          "(the production shape) [regression]") {
    // REGRESSION PIN: production selectedFields never contains "comment" — the legacy field is
    // erased from the picker (EraseCatalogLegacyCommentField), so no view can select it. The wire
    // request still always asks for it (BuildFetchFieldListsFromView hard-codes it into
    // outFieldsList). Keying the mapping off selectedFields silently discarded the payload every
    // sync, leaving BOTH the Comments-cell hover tooltip (fieldValues["comment"]) and its numeric
    // count (fieldValues["comments"]) empty. The mapping must be unconditional.
    nlohmann::json commentBody;
    commentBody["body"] = "A comment";
    commentBody["author"]["displayName"] = "Alice";
    commentBody["created"] = "2024-01-15T12:34:56.000Z";

    nlohmann::json commentObj;
    commentObj["comments"] = nlohmann::json::array({commentBody});
    commentObj["total"] = 1;

    nlohmann::json fields;
    fields["summary"] = "Row without a comment column";
    fields["comment"] = commentObj;

    const nlohmann::json issue = MakeIssue("SMAT-40", fields);
    const std::vector<std::string> selected = {"summary", "status"}; // realistic: no "comment"
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    const std::string blob = GetField(results[0], "comment");
    CHECK(blob.find("Alice") != std::string::npos);
    CHECK(blob.find("A comment") != std::string::npos);
    CHECK(GetField(results[0], "comments") == "1");
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — unconditional comment mapping never fires the "
          "per-row network fallback (NO per-row network during sync) [regression]") {
    // The post-loop mapping passes a NO-OP fetch: with an empty inline array and total > 0,
    // ResolveJiraCommentField's per-issue HTTP top-up must NOT run during sync — the grid
    // cell's one-shot first-hover fetch covers that payload shape instead. The count still maps.
    nlohmann::json commentObj;
    commentObj["comments"] = nlohmann::json::array();
    commentObj["total"] = 2;

    nlohmann::json fields;
    fields["comment"] = commentObj;

    const nlohmann::json issue = MakeIssue("SMAT-42", fields);
    const std::vector<std::string> selected = {"summary"}; // production shape: no "comment"

    bool fetchCalled = false;
    auto liveFetch = [&](const std::string&, nlohmann::json&) -> bool {
        fetchCalled = true;
        return false;
    };

    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, liveFetch, results);

    CHECK(!fetchCalled);
    REQUIRE(results.size() == 1);
    CHECK(GetField(results[0], "comments") == "2");
    CHECK(GetField(results[0], "comment").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — selecting the `comments` count column does not "
          "clobber the derived count [regression]") {
    // The loop's absent-key branch writes "" for `comments` (Jira's payload has no literal
    // `comments` key); the unconditional comment mapping runs AFTER the loop and must win.
    nlohmann::json commentBody;
    commentBody["body"] = "A comment";

    nlohmann::json commentObj;
    commentObj["comments"] = nlohmann::json::array({commentBody});
    commentObj["total"] = 3;

    nlohmann::json fields;
    fields["comment"] = commentObj;

    const nlohmann::json issue = MakeIssue("SMAT-41", fields);
    const std::vector<std::string> selected = {"comments"}; // the synthetic count column
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(GetField(results[0], "comments") == "3");
    CHECK(!GetField(results[0], "comment").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — inline comments used when present") {
    nlohmann::json commentBody;
    commentBody["body"] = "A comment";

    nlohmann::json commentObj;
    commentObj["comments"] = nlohmann::json::array({commentBody});
    commentObj["total"] = 1;

    nlohmann::json fields;
    fields["comment"] = commentObj;

    const nlohmann::json issue = MakeIssue("SMAT-6", fields);
    const std::vector<std::string> selected = {"comment"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(!GetField(results[0], "comment").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — comments fetched lazily when inline empty but total > 0") {
    nlohmann::json commentObj;
    commentObj["comments"] = nlohmann::json::array();
    commentObj["total"] = 2;

    nlohmann::json fields;
    fields["comment"] = commentObj;

    const nlohmann::json issue = MakeIssue("SMAT-7", fields);
    const std::vector<std::string> selected = {"comment"};

    bool fetchCalled = false;
    auto lazyFetch = [&](const std::string& /*key*/, nlohmann::json& out) -> bool {
        fetchCalled = true;
        nlohmann::json c;
        c["body"] = "Lazy comment";
        out = nlohmann::json::array({c});
        return true;
    };

    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, lazyFetch, results);

    CHECK(fetchCalled);
    REQUIRE(results.size() == 1);
    CHECK(!GetField(results[0], "comment").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — ADF doc object stored in fieldRichValues") {
    nlohmann::json adf;
    adf["type"] = "doc";
    adf["version"] = 1;
    adf["content"] = nlohmann::json::array();

    nlohmann::json fields;
    fields["description"] = adf;

    const nlohmann::json issue = MakeIssue("SMAT-8", fields);
    const std::vector<std::string> selected = {"description"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(!GetRichField(results[0], "description").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — custom object field stringified for grid") {
    nlohmann::json customVal;
    customVal["id"] = "option-1";
    customVal["value"] = "Some option";

    nlohmann::json fields;
    fields["customfield_10001"] = customVal;

    const nlohmann::json issue = MakeIssue("SMAT-9", fields);
    const std::vector<std::string> selected = {"customfield_10001"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    // Object should be stringified (not empty, not crash)
    CHECK(!GetField(results[0], "customfield_10001").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — watchers direct field stringified") {
    nlohmann::json fields;
    fields["watchers"] = {{"watchCount", 5}};

    const nlohmann::json issue = MakeIssue("SMAT-W", fields);
    const std::vector<std::string> selected = {"watchers"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(!GetField(results[0], "watchers").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — timetracking object formatted via display helper") {
    nlohmann::json tt;
    tt["originalEstimate"] = "1d";
    tt["timeSpent"] = "2h";

    nlohmann::json fields;
    fields["timetracking"] = tt;

    const nlohmann::json issue = MakeIssue("SMAT-TT", fields);
    const std::vector<std::string> selected = {"timetracking"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(!GetField(results[0], "timetracking").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — duration-seconds integer field formatted") {
    nlohmann::json fields;
    fields["timespent"] = 3600; // 1h in seconds

    const nlohmann::json issue = MakeIssue("SMAT-DS", fields);
    const std::vector<std::string> selected = {"timespent"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    // 3600s formats to a non-empty duration string (not the raw "3600").
    CHECK(!GetField(results[0], "timespent").empty());
    CHECK(GetField(results[0], "timespent") != "3600");
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — attachment array stringified for grid") {
    nlohmann::json fields;
    fields["attachment"] = nlohmann::json::array({{{"filename", "a.png"}}, {{"filename", "b.png"}}});

    const nlohmann::json issue = MakeIssue("SMAT-AT", fields);
    const std::vector<std::string> selected = {"attachment"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(!GetField(results[0], "attachment").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — issuetype always populated from fields") {
    nlohmann::json fields;
    fields["issuetype"] = {{"name", "Bug"}};
    fields["summary"] = "A bug";

    const nlohmann::json issue = MakeIssue("SMAT-10", fields);
    const std::vector<std::string> selected = {"summary"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(!GetField(results[0], "issuetype").empty());
}

TEST_CASE("AppendCachedTicketFromJiraSearchIssue — returns false on completely malformed JSON") {
    // Pass an array (not an object) — causes an exception in the mapper.
    const nlohmann::json issue = nlohmann::json::array({"garbage"});
    const std::vector<std::string> selected = {"summary"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    CHECK_FALSE(ok);
    CHECK(results.empty());
}

// --- BuildFetchFieldListsFromView tests ---

TEST_CASE("BuildFetchFieldListsFromView — empty store produces default field set") {
    ViewsStore store;
    std::vector<std::string> fieldsList;
    std::vector<std::string> selectedFields;
    BuildFetchFieldListsFromView(store, fieldsList, selectedFields);

    CHECK(!fieldsList.empty());
    CHECK(!selectedFields.empty());
    // summary must always be present
    const auto hasField = [](const std::vector<std::string>& v, const std::string& s) {
        for (const auto& x : v)
            if (x == s)
                return true;
        return false;
    };
    CHECK(hasField(selectedFields, "summary"));
    CHECK(hasField(selectedFields, "status"));
    CHECK(hasField(selectedFields, "priority"));
}

TEST_CASE("BuildFetchFieldListsFromView — active view custom fields appended") {
    ViewsStore store;
    ViewDefinition view;
    view.Id = "v1";
    view.Fields = {"summary", "customfield_99999", "assignee"};
    store.Views.push_back(view);
    store.ActiveViewId = "v1";

    std::vector<std::string> fieldsList;
    std::vector<std::string> selectedFields;
    BuildFetchFieldListsFromView(store, fieldsList, selectedFields);

    const auto hasField = [](const std::vector<std::string>& v, const std::string& s) {
        for (const auto& x : v)
            if (x == s)
                return true;
        return false;
    };
    CHECK(hasField(fieldsList, "customfield_99999"));
    CHECK(hasField(selectedFields, "customfield_99999"));
}

TEST_CASE("BuildFetchFieldListsFromView — history field in selectedFields but not fieldsList extras") {
    ViewsStore store;
    ViewDefinition view;
    view.Id = "v1";
    view.Fields = {"summary", "history"};
    store.Views.push_back(view);
    store.ActiveViewId = "v1";

    std::vector<std::string> fieldsList;
    std::vector<std::string> selectedFields;
    BuildFetchFieldListsFromView(store, fieldsList, selectedFields);

    const auto hasField = [](const std::vector<std::string>& v, const std::string& s) {
        for (const auto& x : v)
            if (x == s)
                return true;
        return false;
    };
    // history is a virtual field: goes into selectedFields but not added to fieldsList
    // (changelog is already in fieldsList from the baseline set)
    CHECK(hasField(selectedFields, "history"));
    CHECK(!hasField(fieldsList, "history"));
}

TEST_CASE("BuildFetchFieldListsFromView — duplicate fields in view not duplicated in output") {
    ViewsStore store;
    ViewDefinition view;
    view.Id = "v1";
    view.Fields = {"summary", "summary", "assignee"};
    store.Views.push_back(view);
    store.ActiveViewId = "v1";

    std::vector<std::string> fieldsList;
    std::vector<std::string> selectedFields;
    BuildFetchFieldListsFromView(store, fieldsList, selectedFields);

    int count = 0;
    for (const auto& f : selectedFields)
        if (f == "summary")
            ++count;
    CHECK(count == 1);
}

// --- Slice 0 WS2 of multi-grid-tabs — null / missing-relation / empty-optional
// mapping edges. Characterization only: these pin CURRENT behaviour ahead of the
// multi-pane refactor; a surprising result here is documented, not "fixed".

TEST_CASE("WS2 edge — explicit-null fields map to empty strings without throwing") {
    nlohmann::json fields;
    fields["summary"] = nullptr;
    fields["assignee"] = nullptr;
    fields["priority"] = nullptr;

    const nlohmann::json issue = MakeIssue("SMAT-N1", fields);
    const std::vector<std::string> selected = {"summary", "assignee", "priority"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(GetField(results[0], "summary").empty());
    CHECK(GetField(results[0], "assignee").empty());
    CHECK(GetField(results[0], "priority").empty());
}

TEST_CASE("WS2 edge — explicit-null comment field maps to empty string (no lazy fetch)") {
    nlohmann::json fields;
    fields["comment"] = nullptr;

    const nlohmann::json issue = MakeIssue("SMAT-N2", fields);
    const std::vector<std::string> selected = {"comment"};

    bool fetchCalled = false;
    auto trackingFetch = [&](const std::string&, nlohmann::json&) -> bool {
        fetchCalled = true;
        return false;
    };

    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, trackingFetch, results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(GetField(results[0], "comment").empty());
    CHECK_FALSE(fetchCalled); // null short-circuits before the comments-object branch
}

TEST_CASE("WS2 edge — comment object missing the comments array bypasses the comment resolver") {
    // {"total": 2} without "comments" fails the `contains("comments")` guard and
    // falls into the generic object stringifier — the mapper must not throw.
    nlohmann::json fields;
    fields["comment"] = {{"total", 2}};

    const nlohmann::json issue = MakeIssue("SMAT-N3", fields);
    const std::vector<std::string> selected = {"comment"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].fieldValues.count("comment") == 1);
}

TEST_CASE("WS2 edge — null duration-seconds field formats to empty (not '0h')") {
    nlohmann::json fields;
    fields["timespent"] = nullptr;

    const nlohmann::json issue = MakeIssue("SMAT-N4", fields);
    const std::vector<std::string> selected = {"timespent"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(GetField(results[0], "timespent").empty());
}

TEST_CASE("#943 — keyless AND idless issue is rejected as structurally unmappable") {
    // FIXED (#943): an issue with neither "key" nor "id" has no stable identity and
    // cannot be reconciled downstream — the mapper now rejects it (returns false,
    // appends nothing) instead of silently producing an empty-id ticket.
    nlohmann::json issue;
    issue["fields"] = {{"summary", "No key"}};

    const std::vector<std::string> selected = {"summary"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    CHECK_FALSE(ok);
    CHECK(results.empty());
}

TEST_CASE("#943 — keyless issue with a numeric id falls back to id for the row identity") {
    // An issue missing "key" but carrying "id" is still mappable: the id becomes the
    // ticket identity rather than dropping the row.
    nlohmann::json issue;
    issue["id"] = "10042";
    issue["fields"] = {{"summary", "Has id, no key"}};

    const std::vector<std::string> selected = {"summary"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == "10042");
    CHECK(GetField(results[0], "summary") == "Has id, no key");
}

TEST_CASE("#943 — explicit-null changelog with history selected degrades the column, keeps the row") {
    // FIXED (#943): an explicit-null `changelog` while history is selected no longer
    // drops the whole row. The history column degrades to empty and the row maps.
    nlohmann::json fields;
    fields["summary"] = "Null changelog";

    nlohmann::json issue = MakeIssue("SMAT-N5", fields);
    issue["changelog"] = nullptr;

    const std::vector<std::string> selected = {"summary", "history"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == "SMAT-N5");
    CHECK(GetField(results[0], "summary") == "Null changelog");
    // history column present but degraded to empty.
    CHECK(results[0].fieldValues.count("history") == 1);
    CHECK(GetField(results[0], "history").empty());
}

TEST_CASE("#943 — non-object (non-null) changelog with history selected also degrades, keeps the row") {
    // A changelog that exists but is the wrong type (e.g. a string) must not throw
    // through .value() either — same degrade-not-drop contract.
    nlohmann::json fields;
    fields["summary"] = "Malformed changelog";

    nlohmann::json issue = MakeIssue("SMAT-N5b", fields);
    issue["changelog"] = "oops-not-an-object";

    const std::vector<std::string> selected = {"summary", "history"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].fieldValues.count("history") == 1);
    CHECK(GetField(results[0], "history").empty());
}

TEST_CASE("#943 — missing changelog with history selected maps an empty history column") {
    // No changelog key at all (the common case for a search without expand=changelog)
    // maps history to empty without dropping the row.
    nlohmann::json fields;
    fields["summary"] = "No changelog key";

    const nlohmann::json issue = MakeIssue("SMAT-N5c", fields);
    const std::vector<std::string> selected = {"summary", "history"};
    std::vector<CachedTicket> results;
    const bool ok = AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(ok);
    REQUIRE(results.size() == 1);
    CHECK(results[0].fieldValues.count("history") == 1);
    CHECK(GetField(results[0], "history").empty());
}

TEST_CASE("WS2 edge — array field of nulls collapses to empty string") {
    nlohmann::json fields;
    fields["labels"] = nlohmann::json::array({nullptr, nullptr});

    const nlohmann::json issue = MakeIssue("SMAT-N6", fields);
    const std::vector<std::string> selected = {"labels"};
    std::vector<CachedTicket> results;
    AppendCachedTicketFromJiraSearchIssue(issue, selected, NoCommentFetch(), results);

    REQUIRE(results.size() == 1);
    CHECK(GetField(results[0], "labels").empty());
}

// ---- #670: transition matching prioritises status id/name GLOBALLY ----

using smatchet::jira::FindJiraTransitionId;
using smatchet::jira::JiraTransitionMatch;

TEST_CASE("FindJiraTransitionId — exact to.name beats an earlier transition-name match (#670)") {
    // Transition #11 is *named* "Done" but leads to status "In Review"; transition
    // #21 actually leads to status "Done". A per-transition priority returned #11
    // (name match) → the issue moved to the WRONG status. Global priority must pick #21.
    const nlohmann::json transitions = nlohmann::json::array({
        {{"id", "11"}, {"name", "Done"}, {"to", {{"id", "10001"}, {"name", "In Review"}}}},
        {{"id", "21"}, {"name", "Resolve Issue"}, {"to", {{"id", "10002"}, {"name", "Done"}}}},
    });
    const JiraTransitionMatch m = FindJiraTransitionId(transitions, "", "Done");
    CHECK(m.id == "21");
    CHECK_FALSE(m.usedNameFallback);
}

TEST_CASE("FindJiraTransitionId — status id wins outright") {
    const nlohmann::json transitions = nlohmann::json::array({
        {{"id", "11"}, {"name", "Done"}, {"to", {{"id", "10001"}, {"name", "In Review"}}}},
        {{"id", "21"}, {"name", "Finish"}, {"to", {{"id", "10002"}, {"name", "Done"}}}},
    });
    const JiraTransitionMatch m = FindJiraTransitionId(transitions, "10002", "Done");
    CHECK(m.id == "21");
    CHECK_FALSE(m.usedNameFallback);
}

TEST_CASE("FindJiraTransitionId — transition-name fallback only when no status match exists") {
    // No transition's to.name is "Done"; the only "Done" signal is a transition NAME.
    const nlohmann::json transitions = nlohmann::json::array({
        {{"id", "11"}, {"name", "Start"}, {"to", {{"id", "10001"}, {"name", "In Progress"}}}},
        {{"id", "31"}, {"name", "Done"}, {"to", {{"id", "10009"}, {"name", "Closed"}}}},
    });
    const JiraTransitionMatch m = FindJiraTransitionId(transitions, "", "Done");
    CHECK(m.id == "31");
    CHECK(m.usedNameFallback);
    CHECK(m.toStatusName == "Closed");
}

TEST_CASE("FindJiraTransitionId — no candidate returns empty, integer ids stringified") {
    const nlohmann::json none = nlohmann::json::array({
        {{"id", 41}, {"name", "Reopen"}, {"to", {{"id", 10005}, {"name", "Reopened"}}}},
    });
    CHECK(FindJiraTransitionId(none, "", "Done").id.empty());
    // Integer id forms are accepted and stringified.
    const JiraTransitionMatch m = FindJiraTransitionId(none, "10005", "");
    CHECK(m.id == "41");
}
