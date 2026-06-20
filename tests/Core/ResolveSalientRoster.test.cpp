// Bucket-A coverage for ResolveSalientRoster (ticket-change-monitor plan, S1c-2, item 1
// residual) — resolves the fixed canonical salient roster (Status, Assignee, Priority,
// Summary, DueDate, Sprint, Labels, Components) against one pane's field catalog, mapping each
// role to its concrete backend field id via family → SchemaSystem → exact-name precedence.
// Pure: no backend, pane, or network. The downstream differ is covered in
// TicketChangeDiffPure.test.cpp.

#include "SalientRosterResolve.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using smatchet::ResolveSalientRoster;
using smatchet::SalientFieldRole;

namespace {

TrackerField MakeField(const std::string& id, const std::string& name, const std::string& schemaSystem,
                       TrackerFieldFamily family) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.SchemaSystem = schemaSystem;
    f.Family = family;
    return f;
}

// Convenience: collect labels in roster order for order assertions.
std::vector<std::string> Labels(const std::vector<SalientFieldRole>& roster) {
    std::vector<std::string> out;
    for (size_t i = 0; i < roster.size(); ++i) {
        out.push_back(roster[i].label);
    }
    return out;
}

// Find the field id mapped to a role label (empty if absent).
std::string IdFor(const std::vector<SalientFieldRole>& roster, const std::string& label) {
    for (size_t i = 0; i < roster.size(); ++i) {
        if (roster[i].label == label) {
            return roster[i].fieldId;
        }
    }
    return std::string();
}

} // namespace

TEST_CASE("ResolveSalientRoster: empty catalog yields empty roster") { CHECK(ResolveSalientRoster({}).empty()); }

TEST_CASE("ResolveSalientRoster: Jira system ids resolve every canonical role") {
    const std::vector<TrackerField> fields = {
        MakeField("status", "Status", "status", TrackerFieldFamily::Status),
        MakeField("assignee", "Assignee", "assignee", TrackerFieldFamily::UserSingle),
        MakeField("priority", "Priority", "priority", TrackerFieldFamily::SelectSingle),
        MakeField("summary", "Summary", "summary", TrackerFieldFamily::Text),
        MakeField("duedate", "Due date", "duedate", TrackerFieldFamily::Date),
        MakeField("customfield_10020", "Sprint", "", TrackerFieldFamily::Sprint),
        MakeField("labels", "Labels", "labels", TrackerFieldFamily::Labels),
        MakeField("components", "Components", "components", TrackerFieldFamily::Unknown),
    };
    const std::vector<SalientFieldRole> roster = ResolveSalientRoster(fields);
    // All eight roles resolve, in canonical order.
    CHECK(Labels(roster) == std::vector<std::string>{"Status", "Assignee", "Priority", "Summary", "DueDate", "Sprint",
                                                     "Labels", "Components"});
    CHECK(IdFor(roster, "Sprint") == "customfield_10020"); // resolved by Family, not name/schema
    CHECK(IdFor(roster, "DueDate") == "duedate");
}

TEST_CASE("ResolveSalientRoster: structural family wins for Status/Sprint/Labels") {
    // A backend that surfaces a Status field under a non-"status" id/schema but the Status family.
    const std::vector<TrackerField> fields = {
        MakeField("state", "State", "", TrackerFieldFamily::Status),
        MakeField("custom_sprint", "Iteration", "", TrackerFieldFamily::Sprint),
        MakeField("tags", "Tags", "", TrackerFieldFamily::Labels),
    };
    const std::vector<SalientFieldRole> roster = ResolveSalientRoster(fields);
    CHECK(IdFor(roster, "Status") == "state");
    CHECK(IdFor(roster, "Sprint") == "custom_sprint");
    CHECK(IdFor(roster, "Labels") == "tags");
}

TEST_CASE("ResolveSalientRoster: exact name fallback resolves non-Jira backends") {
    // No Jira system ids, no structural families — only field names match the role.
    const std::vector<TrackerField> fields = {
        MakeField("f1", "priority", "", TrackerFieldFamily::Unknown),
        MakeField("f2", "Due Date", "", TrackerFieldFamily::Unknown), // case-insensitive
        MakeField("f3", "Assignee", "", TrackerFieldFamily::Unknown),
    };
    const std::vector<SalientFieldRole> roster = ResolveSalientRoster(fields);
    CHECK(IdFor(roster, "Priority") == "f1");
    CHECK(IdFor(roster, "DueDate") == "f2");
    CHECK(IdFor(roster, "Assignee") == "f3");
}

TEST_CASE("ResolveSalientRoster: non-salient and substring-only names do not resolve") {
    const std::vector<TrackerField> fields = {
        MakeField("description", "Description", "description", TrackerFieldFamily::Text),
        MakeField("cf_1", "Priority Score", "", TrackerFieldFamily::Number),   // substring, NOT exact
        MakeField("cf_2", "Status Category", "", TrackerFieldFamily::Unknown), // substring, NOT exact
    };
    CHECK(ResolveSalientRoster(fields).empty());
}

TEST_CASE("ResolveSalientRoster: a field binds to at most one role (highest priority)") {
    // A field carrying both the Status family AND a name that could match nothing else still
    // binds once. Construct a field that family-matches Status; ensure it appears exactly once.
    const std::vector<TrackerField> fields = {
        MakeField("status", "Status", "status", TrackerFieldFamily::Status),
    };
    const std::vector<SalientFieldRole> roster = ResolveSalientRoster(fields);
    REQUIRE(roster.size() == 1);
    CHECK(roster[0].label == "Status");
    CHECK(roster[0].fieldId == "status");
}

TEST_CASE("ResolveSalientRoster: blank-id fields are skipped") {
    std::vector<TrackerField> fields = {MakeField("", "Status", "status", TrackerFieldFamily::Status)};
    CHECK(ResolveSalientRoster(fields).empty());
}
