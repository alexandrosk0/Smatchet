// LinearMutationPure doctest — the hermetic half of the Linear write-path
// coverage: pins the exact GraphQL documents, the field→input mapping, priority/
// option resolution, and the {success,entity} / resolve-issue parse, with no
// network. Slice 3 of docs/plans/active/linear-tracker-backend.md.

#include "LinearMutationPure.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace smatchet::linear;

namespace {

// A select-like field carrying option (Id=UUID, Value=display) pairs.
TrackerField MakeField(const std::string& id, const std::vector<std::pair<std::string, std::string>>& options) {
    TrackerField f;
    f.Id = id;
    for (const auto& o : options) {
        TrackerFieldOption opt;
        opt.Id = o.first;
        opt.Value = o.second;
        f.AllowedValueOptions.push_back(opt);
    }
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// GraphQL documents — pin the exact wire strings (a silent edit breaks live).
// ---------------------------------------------------------------------------
TEST_CASE("Linear GraphQL documents are pinned exactly") {
    CHECK(std::string(ResolveIssueQueryDocument()) == "query($id:String!){ issue(id:$id){ id } }");
    CHECK(std::string(IssueUpdateMutationDocument()) ==
          "mutation($id: String!, $input: IssueUpdateInput!){ "
          "issueUpdate(id:$id, input:$input){ success issue{ id identifier } } }");
    CHECK(std::string(IssueCreateMutationDocument()) ==
          "mutation($input: IssueCreateInput!){ "
          "issueCreate(input:$input){ success issue{ id identifier url } } }");
    CHECK(std::string(CommentCreateMutationDocument()) ==
          "mutation($input: CommentCreateInput!){ commentCreate(input:$input){ success comment{ id } } }");
}

// ---------------------------------------------------------------------------
// MapPriorityValueToInt — id and label forms; reject unknown.
// ---------------------------------------------------------------------------
TEST_CASE("MapPriorityValueToInt accepts ids and labels, rejects unknown") {
    int p = -1;
    CHECK((MapPriorityValueToInt("No priority", p) && p == 0));
    CHECK((MapPriorityValueToInt("None", p) && p == 0));
    CHECK((MapPriorityValueToInt("0", p) && p == 0));
    CHECK((MapPriorityValueToInt("Urgent", p) && p == 1));
    CHECK((MapPriorityValueToInt("1", p) && p == 1));
    CHECK((MapPriorityValueToInt("High", p) && p == 2));
    CHECK((MapPriorityValueToInt("Medium", p) && p == 3));
    CHECK((MapPriorityValueToInt("Low", p) && p == 4));
    CHECK((MapPriorityValueToInt("4", p) && p == 4));

    p = 99;
    CHECK_FALSE(MapPriorityValueToInt("Critical", p)); // unknown — must not silently map
    CHECK(p == 99);                                    // out param untouched on failure
}

// ---------------------------------------------------------------------------
// ResolveOptionUuid — UUID passthrough, display→UUID, unmatched passthrough.
// ---------------------------------------------------------------------------
TEST_CASE("ResolveOptionUuid resolves display→UUID and passes UUIDs/unknowns through") {
    const TrackerField status = MakeField("status", {{"uuid-todo", "Todo"}, {"uuid-prog", "In Progress"}});
    CHECK(ResolveOptionUuid(status, "uuid-prog") == "uuid-prog"); // already a UUID
    CHECK(ResolveOptionUuid(status, "In Progress") == "uuid-prog"); // display → UUID
    CHECK(ResolveOptionUuid(status, "Backlog") == "Backlog");       // unmatched — pass through
    const TrackerField noOpts = MakeField("status", {});
    CHECK(ResolveOptionUuid(noOpts, "anything") == "anything"); // no options this session
}

// ---------------------------------------------------------------------------
// BuildIssueUpdateInput — per-field mapping, clears, unknown-field + bad-priority.
// ---------------------------------------------------------------------------
TEST_CASE("BuildIssueUpdateInput maps each editable field to its IssueUpdateInput key") {
    SUBCASE("summary → title") {
        auto r = BuildIssueUpdateInput(MakeField("summary", {}), {"New title"});
        REQUIRE(r);
        CHECK(r.value()["title"] == "New title");
    }
    SUBCASE("description → description") {
        auto r = BuildIssueUpdateInput(MakeField("description", {}), {"## body"});
        REQUIRE(r);
        CHECK(r.value()["description"] == "## body");
    }
    SUBCASE("status → stateId (display resolved)") {
        auto r = BuildIssueUpdateInput(MakeField("status", {{"uuid-done", "Done"}}), {"Done"});
        REQUIRE(r);
        CHECK(r.value()["stateId"] == "uuid-done");
    }
    SUBCASE("assignee empty clears to null") {
        auto r = BuildIssueUpdateInput(MakeField("assignee", {}), {""});
        REQUIRE(r);
        CHECK(r.value()["assigneeId"].is_null());
    }
    SUBCASE("assignee set resolves display→UUID") {
        auto r = BuildIssueUpdateInput(MakeField("assignee", {{"uuid-alice", "Alice"}}), {"Alice"});
        REQUIRE(r);
        CHECK(r.value()["assigneeId"] == "uuid-alice");
    }
    SUBCASE("labels is set-replace array of resolved ids") {
        auto r = BuildIssueUpdateInput(MakeField("labels", {{"uuid-bug", "bug"}, {"uuid-p1", "p1"}}), {"bug", "p1"});
        REQUIRE(r);
        REQUIRE(r.value()["labelIds"].is_array());
        CHECK(r.value()["labelIds"].size() == 2);
        CHECK(r.value()["labelIds"][0] == "uuid-bug");
        CHECK(r.value()["labelIds"][1] == "uuid-p1");
    }
    SUBCASE("priority label → int") {
        auto r = BuildIssueUpdateInput(MakeField("priority", {}), {"High"});
        REQUIRE(r);
        CHECK(r.value()["priority"] == 2);
    }
    SUBCASE("priority empty clears to 0 (No priority)") {
        auto r = BuildIssueUpdateInput(MakeField("priority", {}), {""});
        REQUIRE(r);
        CHECK(r.value()["priority"] == 0);
    }
    SUBCASE("priority unknown → Err") {
        auto r = BuildIssueUpdateInput(MakeField("priority", {}), {"Critical"});
        CHECK_FALSE(r);
    }
    SUBCASE("project empty clears to null") {
        auto r = BuildIssueUpdateInput(MakeField("project", {}), {""});
        REQUIRE(r);
        CHECK(r.value()["projectId"].is_null());
    }
    SUBCASE("unknown field → Err (not editable on Linear)") {
        auto r = BuildIssueUpdateInput(MakeField("storyPoints", {}), {"5"});
        CHECK_FALSE(r);
    }
}

// ---------------------------------------------------------------------------
// BuildLabelIdsFromCsv — trim, skip blanks, resolve via options.
// ---------------------------------------------------------------------------
TEST_CASE("BuildLabelIdsFromCsv trims, drops blanks, resolves display→UUID") {
    const TrackerField labels = MakeField("labels", {{"uuid-bug", "bug"}, {"uuid-p1", "p1"}});
    const nlohmann::json ids = BuildLabelIdsFromCsv("  bug , ,p1 ", &labels);
    REQUIRE(ids.is_array());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "uuid-bug");
    CHECK(ids[1] == "uuid-p1");

    // Null field → pass-through tokens (still trimmed/blank-dropped).
    const nlohmann::json passthrough = BuildLabelIdsFromCsv("a,,b", nullptr);
    REQUIRE(passthrough.size() == 2);
    CHECK(passthrough[0] == "a");
    CHECK(passthrough[1] == "b");

    CHECK(BuildLabelIdsFromCsv("   ", nullptr).empty());
}

// ---------------------------------------------------------------------------
// BuildIssueCreateInput — teamId/title required; full build resolves options.
// ---------------------------------------------------------------------------
TEST_CASE("BuildIssueCreateInput requires teamId and title") {
    IssueDraft draft;
    draft.FieldValues["summary"] = "Hello";
    CHECK_FALSE(BuildIssueCreateInput(draft, {}, "")); // no team

    IssueDraft noTitle;
    CHECK_FALSE(BuildIssueCreateInput(noTitle, {}, "team-uuid")); // no summary
}

TEST_CASE("BuildIssueCreateInput builds a full IssueCreateInput with option resolution") {
    IssueDraft draft;
    draft.FieldValues["summary"] = "Crash on launch";
    draft.FieldValues["description"] = "## repro";
    draft.FieldValues["priority"] = "Urgent";
    draft.FieldValues["status"] = "In Progress";
    draft.FieldValues["assignee"] = "Alice";
    draft.FieldValues["labels"] = "bug, p1";

    std::vector<TrackerField> catalog;
    catalog.push_back(MakeField("status", {{"uuid-prog", "In Progress"}}));
    catalog.push_back(MakeField("assignee", {{"uuid-alice", "Alice"}}));
    catalog.push_back(MakeField("labels", {{"uuid-bug", "bug"}, {"uuid-p1", "p1"}}));

    auto r = BuildIssueCreateInput(draft, catalog, "team-uuid");
    REQUIRE(r);
    const nlohmann::json& in = r.value();
    CHECK(in["teamId"] == "team-uuid");
    CHECK(in["title"] == "Crash on launch");
    CHECK(in["description"] == "## repro");
    CHECK(in["priority"] == 1); // Urgent
    CHECK(in["stateId"] == "uuid-prog");
    CHECK(in["assigneeId"] == "uuid-alice");
    REQUIRE(in["labelIds"].is_array());
    CHECK(in["labelIds"].size() == 2);
    CHECK(in["labelIds"][0] == "uuid-bug");

    // Optional fields stay absent when the draft omits them.
    IssueDraft minimal;
    minimal.FieldValues["summary"] = "Just a title";
    auto m = BuildIssueCreateInput(minimal, {}, "team-uuid");
    REQUIRE(m);
    CHECK(m.value().contains("title"));
    CHECK_FALSE(m.value().contains("description"));
    CHECK_FALSE(m.value().contains("priority"));
    CHECK_FALSE(m.value().contains("labelIds"));
}

// ---------------------------------------------------------------------------
// BuildCommentCreateInput.
// ---------------------------------------------------------------------------
TEST_CASE("BuildCommentCreateInput carries issueId + body verbatim") {
    const nlohmann::json in = BuildCommentCreateInput("issue-uuid", "**bold** comment");
    CHECK(in["issueId"] == "issue-uuid");
    CHECK(in["body"] == "**bold** comment");
}

// ---------------------------------------------------------------------------
// ParseMutationSucceeded — success flag + nested issue extraction; defensive.
// ---------------------------------------------------------------------------
TEST_CASE("ParseMutationSucceeded reads success and extracts the issue node") {
    nlohmann::json issue;
    const nlohmann::json ok = nlohmann::json::parse(
        R"({"data":{"issueUpdate":{"success":true,"issue":{"id":"u1","identifier":"ENG-9"}}}})");
    CHECK(ParseMutationSucceeded(ok, "issueUpdate", issue));
    CHECK(issue["identifier"] == "ENG-9");

    const nlohmann::json notOk =
        nlohmann::json::parse(R"({"data":{"issueUpdate":{"success":false}}})");
    CHECK_FALSE(ParseMutationSucceeded(notOk, "issueUpdate", issue));
    CHECK(issue.is_null()); // reset when no issue node present

    // commentCreate carries `comment`, not `issue` — success true, issue stays null.
    const nlohmann::json comment =
        nlohmann::json::parse(R"({"data":{"commentCreate":{"success":true,"comment":{"id":"c1"}}}})");
    CHECK(ParseMutationSucceeded(comment, "commentCreate", issue));
    CHECK(issue.is_null());

    // Defensive: wrong shapes never throw, return false.
    CHECK_FALSE(ParseMutationSucceeded(nlohmann::json::parse("[]"), "issueUpdate", issue));
    CHECK_FALSE(ParseMutationSucceeded(nlohmann::json::parse(R"({"data":{}})"), "issueUpdate", issue));
}

// ---------------------------------------------------------------------------
// ParseResolvedIssueUuid — found / missing / empty-id.
// ---------------------------------------------------------------------------
TEST_CASE("ParseResolvedIssueUuid extracts the uuid or reports a precise error") {
    std::string err;
    const nlohmann::json found = nlohmann::json::parse(R"({"data":{"issue":{"id":"uuid-1"}}})");
    CHECK(ParseResolvedIssueUuid(found, "ENG-1", err) == "uuid-1");
    CHECK(err.empty());

    err.clear();
    const nlohmann::json missing = nlohmann::json::parse(R"({"data":{"issue":null}})");
    CHECK(ParseResolvedIssueUuid(missing, "ENG-2", err).empty());
    CHECK(err.find("ENG-2") != std::string::npos);
    CHECK(err.find("not found") != std::string::npos);

    err.clear();
    const nlohmann::json blank = nlohmann::json::parse(R"({"data":{"issue":{"id":""}}})");
    CHECK(ParseResolvedIssueUuid(blank, "ENG-3", err).empty());
    CHECK(err.find("empty UUID") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ParseCreatedIssueIdentifier.
// ---------------------------------------------------------------------------
TEST_CASE("ParseCreatedIssueIdentifier returns the identifier or empty") {
    CHECK(ParseCreatedIssueIdentifier(nlohmann::json::parse(R"({"identifier":"ENG-42","id":"u"})")) == "ENG-42");
    CHECK(ParseCreatedIssueIdentifier(nlohmann::json::parse(R"({"id":"u"})")).empty());
    CHECK(ParseCreatedIssueIdentifier(nlohmann::json()).empty());
}
