#include <doctest/doctest.h>

#include "IssueDraft.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

TrackerField MakeIssueTypeField() {
    TrackerField f;
    f.Id = "issuetype";
    f.Name = "Issue Type";
    f.Type = "issuetype";
    TrackerFieldOption bug;
    bug.Id = "10001";
    bug.Value = "Bug";
    TrackerFieldOption story;
    story.Id = "10002";
    story.Value = "Story";
    f.AllowedValueOptions.push_back(bug);
    f.AllowedValueOptions.push_back(story);
    return f;
}

CachedTicket MakeTicket(const std::string& id) {
    CachedTicket t;
    t.id = id;
    return t;
}

} // namespace

TEST_CASE("IssueDraftHelpers::ResolveIssueTypeIdFromTicket resolves by display name") {
    CachedTicket t = MakeTicket("PROJ-42");
    t.fieldValues["issuetype"] = "Bug";
    std::vector<TrackerField> catalog;
    catalog.push_back(MakeIssueTypeField());

    std::string outName;
    const std::string id = IssueDraftHelpers::ResolveIssueTypeIdFromTicket(t, catalog, &outName);
    CHECK(id == "10001");
    CHECK(outName == "Bug");
}

TEST_CASE("IssueDraftHelpers::ResolveIssueTypeIdFromTicket falls back to value when id missing") {
    TrackerField f;
    f.Id = "issuetype";
    TrackerFieldOption epic;
    epic.Id.clear(); // intentionally no id; only display value
    epic.Value = "Epic";
    f.AllowedValueOptions.push_back(epic);

    CachedTicket t = MakeTicket("PROJ-1");
    t.fieldValues["issuetype"] = "Epic";
    std::vector<TrackerField> catalog;
    catalog.push_back(f);

    std::string outName;
    const std::string id = IssueDraftHelpers::ResolveIssueTypeIdFromTicket(t, catalog, &outName);
    CHECK(id == "Epic");
    CHECK(outName == "Epic");
}

TEST_CASE("IssueDraftHelpers::ResolveIssueTypeIdFromTicket returns empty for unknown issue type") {
    CachedTicket t = MakeTicket("PROJ-1");
    t.fieldValues["issuetype"] = "Mystery";
    std::vector<TrackerField> catalog;
    catalog.push_back(MakeIssueTypeField());

    std::string outName;
    const std::string id = IssueDraftHelpers::ResolveIssueTypeIdFromTicket(t, catalog, &outName);
    CHECK(id == "");
    CHECK(outName == "Mystery");
}

TEST_CASE("IssueDraftHelpers::ResolveIssueTypeIdFromTicket returns empty for empty issuetype field") {
    CachedTicket t = MakeTicket("PROJ-1");
    std::vector<TrackerField> catalog;
    catalog.push_back(MakeIssueTypeField());

    std::string outName;
    const std::string id = IssueDraftHelpers::ResolveIssueTypeIdFromTicket(t, catalog, &outName);
    CHECK(id == "");
    CHECK(outName == "");
}

TEST_CASE("IssueDraftHelpers::FromCachedTicket inherits only listed fields") {
    CachedTicket prev = MakeTicket("PROJ-7");
    prev.fieldValues["summary"] = "old summary";       // never inherited
    prev.fieldValues["priority"] = "High";             // inherited (in list)
    prev.fieldValues["assignee"] = "u@example.com";    // inherited (in list)
    prev.fieldValues["customfield_10042"] = "ignored"; // not in list

    std::vector<TrackerField> catalog;
    catalog.push_back(MakeIssueTypeField());

    std::vector<std::string> inherit = {"priority", "assignee"};
    IssueDraft d = IssueDraftHelpers::FromCachedTicket(prev, catalog, "PROJ", "10001", "Bug", inherit);

    CHECK(d.ProjectKey == "PROJ");
    CHECK(d.IssueTypeId == "10001");
    CHECK(d.FieldValues.count("summary") == 0);
    CHECK(d.FieldValues.count("priority") == 1);
    CHECK(d.FieldValues["priority"] == "High");
    CHECK(d.FieldValues.count("assignee") == 1);
    CHECK(d.FieldValues.count("customfield_10042") == 0);
}

TEST_CASE("IssueDraftHelpers::FromCachedTicket skips create-suppressed and special draft ids") {
    CachedTicket prev = MakeTicket("PROJ-7");
    prev.fieldValues["status"] = "Open";       // suppressed
    prev.fieldValues["created"] = "yesterday"; // suppressed
    prev.fieldValues["resolution"] = "Done";   // not suppressed but not in list
    prev.fieldValues["project"] = "OLD";       // special — skipped under default inherit
    std::vector<TrackerField> catalog;
    std::vector<std::string> inherit; // empty -> use default list

    IssueDraft d = IssueDraftHelpers::FromCachedTicket(prev, catalog, "FALLBACK", "10001", "Bug", inherit);

    CHECK(d.FieldValues.count("status") == 0);
    CHECK(d.FieldValues.count("created") == 0);
    CHECK(d.FieldValues.count("project") == 0);
    CHECK(d.ProjectKey == "PROJ"); // derived from ticket.id "PROJ-7"
}

TEST_CASE("IssueDraftHelpers::FromCachedTicket derives project key per backend id shape") {
    std::vector<TrackerField> catalog;
    std::vector<std::string> inherit; // empty -> default inherit (takes project from row)

    SUBCASE("Jira/Linear/Plane KEY-NUM splits on first dash") {
        IssueDraft d = IssueDraftHelpers::FromCachedTicket(MakeTicket("PROJ-7"), catalog, "F", "1", "Bug", inherit);
        CHECK(d.ProjectKey == "PROJ");
    }
    SUBCASE("DR18: GitHub owner/repo#N keeps the dash-containing repo name") {
        // Splitting on the first '-' would corrupt this to "acme/react"; the create
        // POST would then target the wrong repository. Project key must be owner/repo.
        IssueDraft d =
            IssueDraftHelpers::FromCachedTicket(MakeTicket("acme/react-native#12"), catalog, "F", "1", "Bug", inherit);
        CHECK(d.ProjectKey == "acme/react-native");
    }
    SUBCASE("GitHub commit row owner/repo@sha") {
        IssueDraft d =
            IssueDraftHelpers::FromCachedTicket(MakeTicket("acme/web-app@abc123"), catalog, "F", "1", "Bug", inherit);
        CHECK(d.ProjectKey == "acme/web-app");
    }
}

TEST_CASE("IssueDraftHelpers::FromCachedTicket uses fallbacks when ticket is empty") {
    CachedTicket empty;
    std::vector<TrackerField> catalog;
    std::vector<std::string> inherit;

    IssueDraft d = IssueDraftHelpers::FromCachedTicket(empty, catalog, "NEWPROJ", "10005", "Task", inherit);

    CHECK(d.ProjectKey == "NEWPROJ");
    CHECK(d.IssueTypeId == "10005");
    CHECK(d.IssueTypeName == "Task");
    CHECK(d.ParentKey == "");
    CHECK(d.FieldValues.empty());
}

TEST_CASE("IssueDraftHelpers::FromCachedTicket extracts parent key prefix") {
    CachedTicket prev = MakeTicket("PROJ-9");
    prev.fieldValues["parent"] = "PROJ-100 - Some summary";
    std::vector<TrackerField> catalog;
    std::vector<std::string> inherit;

    IssueDraft d = IssueDraftHelpers::FromCachedTicket(prev, catalog, "PROJ", "10001", "Bug", inherit);
    CHECK(d.ParentKey == "PROJ-100");
}

TEST_CASE("IssueDraftHelpers::MissingRequiredFields reports hard minimums") {
    IssueDraft d;
    RequiredFieldSet req;
    req.RequiresIssueType = true;

    std::vector<std::string> missing = IssueDraftHelpers::MissingRequiredFields(d, req);
    CHECK(std::find(missing.begin(), missing.end(), "__project__") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "summary") != missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "__issuetype__") != missing.end());
}

TEST_CASE("IssueDraftHelpers::MissingRequiredFields reports parent for subtasks") {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.FieldValues["summary"] = "S";
    RequiredFieldSet req;
    req.IsSubtask = true;
    req.RequiresIssueType = true;

    std::vector<std::string> missing = IssueDraftHelpers::MissingRequiredFields(d, req);
    CHECK(std::find(missing.begin(), missing.end(), "__parent__") != missing.end());
}

TEST_CASE("IssueDraftHelpers::MissingRequiredFields skips suppressed and special ids") {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.FieldValues["summary"] = "S";
    RequiredFieldSet req;
    req.RequiresIssueType = true;
    req.FieldIds.insert("status");  // suppressed -> skipped
    req.FieldIds.insert("project"); // special -> skipped
    req.FieldIds.insert("customfield_10001");

    std::vector<std::string> missing = IssueDraftHelpers::MissingRequiredFields(d, req);
    CHECK(std::find(missing.begin(), missing.end(), "status") == missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "project") == missing.end());
    CHECK(std::find(missing.begin(), missing.end(), "customfield_10001") != missing.end());
}

TEST_CASE("IssueDraftHelpers::MissingRequiredFields returns empty when ExistingIssueKey is set") {
    IssueDraft d;
    d.ExistingIssueKey = "PROJ-100";
    RequiredFieldSet req;
    req.FieldIds.insert("customfield_10001");
    req.RequiresIssueType = true;

    std::vector<std::string> missing = IssueDraftHelpers::MissingRequiredFields(d, req);
    CHECK(missing.empty());
}

TEST_CASE("IssueDraftHelpers::ToJson and FromJson round-trip preserves string fields") {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.IssueTypeName = "Bug";
    d.ParentKey = "PROJ-100";
    d.FieldValues["summary"] = "hello";
    d.FieldValues["customfield_10042"] = "value with spaces";
    StagedAttachment att;
    att.AbsPath = "C:/tmp/x.png";
    att.FileName = "x.png";
    att.SizeBytes = 12345;
    d.StagedAttachments.push_back(att);

    const std::string json = IssueDraftHelpers::ToJson(d);

    IssueDraft d2;
    std::string err;
    REQUIRE(IssueDraftHelpers::FromJson(json, d2, err));
    CHECK(err.empty());
    CHECK(d2.ProjectKey == "PROJ");
    CHECK(d2.IssueTypeId == "10001");
    CHECK(d2.IssueTypeName == "Bug");
    CHECK(d2.ParentKey == "PROJ-100");
    CHECK(d2.FieldValues["summary"] == "hello");
    CHECK(d2.FieldValues["customfield_10042"] == "value with spaces");
    REQUIRE(d2.StagedAttachments.size() == 1);
    CHECK(d2.StagedAttachments[0].FileName == "x.png");
    CHECK(d2.StagedAttachments[0].SizeBytes == 12345);
}

TEST_CASE("IssueDraftHelpers: ToJsonFromJson_RoundTripPreservesNumericTypes [high-risk]") {
    // Guard: FromJson must coerce JSON-number values back to strings (the in-memory model
    // is stringly-typed). Drift on this path silently corrupts numeric custom-field values
    // queued offline.
    const std::string injected = R"({
        "projectKey": "P",
        "issueTypeId": "1",
        "issueTypeName": "Bug",
        "parentKey": "",
        "fields": {
            "customfield_int": 42,
            "customfield_float": 3.5,
            "customfield_bool": true,
            "customfield_str": "abc"
        },
        "attachments": []
    })";

    IssueDraft d;
    std::string err;
    REQUIRE(IssueDraftHelpers::FromJson(injected, d, err));
    CHECK(d.FieldValues["customfield_int"] == "42");
    CHECK(d.FieldValues["customfield_bool"] == "true");
    CHECK(d.FieldValues["customfield_str"] == "abc");
    // Float formatting may differ across compilers; just ensure non-empty + numeric leading char.
    const std::string& floatStr = d.FieldValues["customfield_float"];
    CHECK_FALSE(floatStr.empty());
    CHECK((floatStr[0] == '3'));
}

TEST_CASE("IssueDraftHelpers::FromJson reports parse errors gracefully") {
    IssueDraft d;
    std::string err;
    CHECK_FALSE(IssueDraftHelpers::FromJson("{not json", d, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("IssueDraftHelpers::ComputeFieldChanges reports only differing fields, trimmed") {
    IssueDraft d;
    d.FieldValues["summary"] = "  new summary  ";
    d.FieldValues["priority"] = "High";
    d.FieldValues["assignee"] = "same"; // matches existing -> no change

    CachedTicket existing = MakeTicket("PROJ-1");
    existing.fieldValues["summary"] = "old summary";
    existing.fieldValues["priority"] = "High";
    existing.fieldValues["assignee"] = "same";

    auto changes = IssueDraftHelpers::ComputeFieldChanges(d, existing);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].FieldId == "summary");
    CHECK(changes[0].OldValue == "old summary");
    CHECK(changes[0].NewValue == "new summary");
}

TEST_CASE("IssueDraftHelpers::ComputeFieldChanges includes parent change") {
    IssueDraft d;
    d.ParentKey = "PROJ-200";
    CachedTicket existing = MakeTicket("PROJ-1");
    existing.fieldValues["parent"] = "PROJ-100";

    auto changes = IssueDraftHelpers::ComputeFieldChanges(d, existing);
    REQUIRE_FALSE(changes.empty());
    auto it = std::find_if(changes.begin(), changes.end(),
                           [](const IssueDraftHelpers::FieldChange& c) { return c.FieldId == "parent"; });
    REQUIRE(it != changes.end());
    CHECK(it->OldValue == "PROJ-100");
    CHECK(it->NewValue == "PROJ-200");
}

TEST_CASE("IssueDraftHelpers::ComputeFieldChanges skips suppressed and synthetic ids") {
    IssueDraft d;
    d.FieldValues["__project__"] = "X";
    d.FieldValues["created"] = "yesterday";
    d.FieldValues["key"] = "PROJ-1";
    d.FieldValues["issuekey"] = "PROJ-1";
    d.FieldValues["summary"] = "diff";

    CachedTicket existing = MakeTicket("PROJ-1");
    existing.fieldValues["summary"] = "orig";

    auto changes = IssueDraftHelpers::ComputeFieldChanges(d, existing);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].FieldId == "summary");
}

TEST_CASE("IssueDraftHelpers::PruneUnchangedFields removes matches but keeps draft routing") {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.IssueTypeName = "Bug";
    d.ParentKey = "PROJ-100";
    d.FieldValues["summary"] = "same";
    d.FieldValues["priority"] = "Higher";

    CachedTicket existing = MakeTicket("PROJ-1");
    existing.fieldValues["summary"] = "same";
    existing.fieldValues["priority"] = "Low";
    existing.fieldValues["parent"] = "PROJ-100"; // matches draft.ParentKey -> cleared

    IssueDraftHelpers::PruneUnchangedFields(d, existing);

    CHECK(d.FieldValues.count("summary") == 0);
    CHECK(d.FieldValues["priority"] == "Higher");
    CHECK(d.ParentKey == ""); // cleared because matched
    CHECK(d.ProjectKey == "PROJ");
    CHECK(d.IssueTypeId == "10001");
    CHECK(d.IssueTypeName == "Bug");
}

TEST_CASE("IssueDraftHelpers: PruneUnchangedFields_DoesNotEraseOnCacheMiss [high-risk]") {
    // Cache-miss safety: when the cached ticket has NO entry for a field but the draft has a
    // non-empty value, the draft value must be preserved (treated as an implicit add). This
    // guards the silent-loss path where a partially-loaded cache would wipe queued edits.
    IssueDraft d;
    d.FieldValues["customfield_new"] = "fresh value";
    d.FieldValues["other"] = "also new";

    CachedTicket existing = MakeTicket("PROJ-1");
    // existing.fieldValues is empty: nothing to compare against
    REQUIRE(existing.fieldValues.empty());

    IssueDraftHelpers::PruneUnchangedFields(d, existing);

    CHECK(d.FieldValues.count("customfield_new") == 1);
    CHECK(d.FieldValues["customfield_new"] == "fresh value");
    CHECK(d.FieldValues.count("other") == 1);
    CHECK(d.FieldValues["other"] == "also new");
}

TEST_CASE("IssueDraftHelpers::PruneUnchangedFields removes empty draft when cache empty") {
    IssueDraft d;
    d.FieldValues["empty_match"] = "";
    CachedTicket existing = MakeTicket("PROJ-1");
    // existing has no 'empty_match' -> baseline is "" -> "" == "" -> prune.
    IssueDraftHelpers::PruneUnchangedFields(d, existing);
    CHECK(d.FieldValues.count("empty_match") == 0);
}

TEST_CASE("IssueDraftHelpers::MapFieldIdsToNames maps synthetic markers and catalog names") {
    std::vector<std::string> ids = {"__project__", "__issuetype__", "__parent__", "priority", "unknown_id"};
    TrackerField priority;
    priority.Id = "priority";
    priority.Name = "Priority";
    std::vector<TrackerField> catalog;
    catalog.push_back(priority);

    auto names = IssueDraftHelpers::MapFieldIdsToNames(ids, catalog);
    REQUIRE(names.size() == 5);
    CHECK(names[0] == "Project");
    CHECK(names[1] == "Issue Type");
    CHECK(names[2] == "Parent");
    CHECK(names[3] == "Priority");
    CHECK(names[4] == "unknown_id");
}

TEST_CASE("IssueDraftHelpers::IsCreateSuppressedFieldId knows server-managed ids") {
    CHECK(IssueDraftHelpers::IsCreateSuppressedFieldId("status"));
    CHECK(IssueDraftHelpers::IsCreateSuppressedFieldId("created"));
    CHECK(IssueDraftHelpers::IsCreateSuppressedFieldId("creator"));
    CHECK(IssueDraftHelpers::IsCreateSuppressedFieldId("worklog"));
    CHECK_FALSE(IssueDraftHelpers::IsCreateSuppressedFieldId("summary"));
    CHECK_FALSE(IssueDraftHelpers::IsCreateSuppressedFieldId("customfield_10042"));
}

TEST_CASE("IssueDraftHelpers::IsSpecialDraftFieldId knows project / issuetype / parent") {
    CHECK(IssueDraftHelpers::IsSpecialDraftFieldId("project"));
    CHECK(IssueDraftHelpers::IsSpecialDraftFieldId("issuetype"));
    CHECK(IssueDraftHelpers::IsSpecialDraftFieldId("parent"));
    CHECK_FALSE(IssueDraftHelpers::IsSpecialDraftFieldId("summary"));
    CHECK_FALSE(IssueDraftHelpers::IsSpecialDraftFieldId("__project__"));
}

TEST_CASE("IssueDraftHelpers::DefaultNewIssueInheritFieldIds returns the canonical list") {
    auto ids = IssueDraftHelpers::DefaultNewIssueInheritFieldIds();
    CHECK_FALSE(ids.empty());
    CHECK(std::find(ids.begin(), ids.end(), "issuetype") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "priority") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "labels") != ids.end());
}
