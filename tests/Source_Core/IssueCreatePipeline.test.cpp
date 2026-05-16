#include <doctest/doctest.h>

#include "IssueCreatePipelineHelpers.h"
#include "IssueDraft.h"

#include <nlohmann/json.hpp>

#include <string>

using IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate;

namespace {

CachedTicket MakeExisting(const std::string& id) {
    CachedTicket t;
    t.id = id;
    return t;
}

} // namespace

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate stamps issue key") {
    CachedTicket existing = MakeExisting("OLD-1");
    IssueDraft draft;
    nlohmann::json fields = nlohmann::json::object();

    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-100", fields);
    CHECK(merged.id == "PROJ-100");
}

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate carries draft IssueTypeName") {
    CachedTicket existing = MakeExisting("PROJ-1");
    existing.fieldValues["issuetype"] = "Bug";
    IssueDraft draft;
    draft.IssueTypeName = "Story";
    nlohmann::json fields = nlohmann::json::object();

    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", fields);
    CHECK(merged.fieldValues["issuetype"] == "Story");
}

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate falls back to IssueTypeId") {
    CachedTicket existing = MakeExisting("PROJ-1");
    IssueDraft draft;
    draft.IssueTypeId = "10001";
    // No IssueTypeName -> id is written instead.
    nlohmann::json fields = nlohmann::json::object();

    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", fields);
    CHECK(merged.fieldValues["issuetype"] == "10001");
}

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate carries ParentKey") {
    CachedTicket existing = MakeExisting("PROJ-1");
    IssueDraft draft;
    draft.ParentKey = "PROJ-99";
    nlohmann::json fields = nlohmann::json::object();

    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", fields);
    CHECK(merged.fieldValues["parent"] == "PROJ-99");
}

TEST_CASE("IssueCreatePipeline: MergeDraftIntoCachedTicketForUpdate_HonoursPutFieldsSucceeded [high-risk]") {
    // Guard: only fields keyed in the PUT-succeeded payload may be overwritten in the cache.
    // Fields the user touched but Jira rejected (omitted from putFieldsSucceeded) must NOT be
    // written back, or the local cache will drift from the server.
    CachedTicket existing = MakeExisting("PROJ-1");
    existing.fieldValues["summary"] = "old summary";
    existing.fieldValues["priority"] = "Low";
    existing.fieldValues["assignee"] = "alice@example.com";

    IssueDraft draft;
    draft.FieldValues["summary"] = "new summary";      // succeeded -> overwrite
    draft.FieldValues["priority"] = "High";            // NOT in putFieldsSucceeded -> stays at "Low"
    draft.FieldValues["assignee"] = "bob@example.com"; // succeeded -> overwrite

    nlohmann::json putFieldsSucceeded = nlohmann::json::object();
    putFieldsSucceeded["summary"] = "anything"; // value isn't read; only the keys matter
    putFieldsSucceeded["assignee"] = nlohmann::json(nullptr);

    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", putFieldsSucceeded);
    CHECK(merged.fieldValues["summary"] == "new summary");
    CHECK(merged.fieldValues["assignee"] == "bob@example.com");
    // Critical: priority was in the draft but NOT in putFieldsSucceeded — must be untouched.
    CHECK(merged.fieldValues["priority"] == "Low");
}

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate skips keys absent from draft") {
    CachedTicket existing = MakeExisting("PROJ-1");
    existing.fieldValues["summary"] = "kept";
    IssueDraft draft;
    // draft has no 'summary' value; putFieldsSucceeded names it but lookup misses.
    nlohmann::json putFieldsSucceeded = nlohmann::json::object();
    putFieldsSucceeded["summary"] = "ignored";

    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", putFieldsSucceeded);
    CHECK(merged.fieldValues["summary"] == "kept");
}

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate tolerates non-object putFields") {
    CachedTicket existing = MakeExisting("PROJ-1");
    existing.fieldValues["summary"] = "kept";
    IssueDraft draft;
    draft.FieldValues["summary"] = "new";

    // Whatever the caller passes that isn't an object: no overlay.
    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", nlohmann::json::array());
    CHECK(merged.fieldValues["summary"] == "kept");

    CachedTicket merged2 = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", nlohmann::json(nullptr));
    CHECK(merged2.fieldValues["summary"] == "kept");
}

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate preserves other existing fields") {
    CachedTicket existing = MakeExisting("PROJ-1");
    existing.fieldValues["status"] = "In Progress";
    existing.fieldValues["reporter"] = "alice";
    IssueDraft draft;
    draft.FieldValues["summary"] = "new";
    nlohmann::json putFieldsSucceeded = nlohmann::json::object();
    putFieldsSucceeded["summary"] = "x";

    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", putFieldsSucceeded);
    CHECK(merged.fieldValues["status"] == "In Progress");
    CHECK(merged.fieldValues["reporter"] == "alice");
    CHECK(merged.fieldValues["summary"] == "new");
}

TEST_CASE("IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate does not write issuetype when both blank") {
    CachedTicket existing = MakeExisting("PROJ-1");
    IssueDraft draft;
    // No IssueTypeName, no IssueTypeId -> issuetype not added.
    nlohmann::json fields = nlohmann::json::object();
    CachedTicket merged = MergeDraftIntoCachedTicketForUpdate(existing, draft, "PROJ-1", fields);
    CHECK(merged.fieldValues.count("issuetype") == 0);
}

// ApplyPostIssueSteps decision-logic coverage is deferred — that helper invokes
// ITrackerClient::UpdateIssueFields / AddIssueToSprint / AttachFilesToIssue and
// is wedded to the HTTP path. Lifting its decision math into a pure free function
// requires a mock-client interface that doesn't exist yet. Tracked separately so
// Phase 1 can land without an HTTP-stub refactor.
