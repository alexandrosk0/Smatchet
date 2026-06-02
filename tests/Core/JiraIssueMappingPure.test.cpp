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
