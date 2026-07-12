// IssueCreatePipeline end-to-end integration tests.
//
// Pairs a scripted `FakeTrackerClient` with the in-memory `FakeSyncCache` (ADR-0020 /
// ilocalcache-seam — contract-suite-verified against the real cache) so each test runs
// the full IssueCreatePipeline::Run flow without touching network, disk, OR SQLite. Covers:
//   * Create path — POST succeeds, cache row seeded.
//   * Update path — PUT succeeds, cache row merged via MergeDraftIntoCachedTicketForUpdate.
//   * Failure mapping — BuildCreatePayload error, CreateIssue error, UpdateIssueFields error.
//   * Missing-required-field validation.
//   * Status / sprint post-create steps (best-effort, not gating Ok).
//   * Attachment-failure pass-through.

#include "../support/FakeTrackerClient.h"
#include "../support/FakeSyncCache.h"

#include "ConfigManager.h"
#include "ITrackerCollaboration.h"
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "ISyncCache.h"
#include "TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet_tests::FakeSyncCache;
using smatchet_tests::FakeTrackerClient;

namespace {

// Drop-in replacement for the retired SqliteMemFixture usage here: same Get()/Ref() surface,
// backed by the contract-suite-verified in-memory fake instead of :memory: SQLite.
class FakeCacheFixture {
  public:
    ISyncCache* Get() { return &cache_; }
    ISyncCache& Ref() { return cache_; }

  private:
    FakeSyncCache cache_;
};

TrackerField MakeField(const std::string& id, TrackerFieldFamily family = TrackerFieldFamily::Text) {
    TrackerField f;
    f.Id = id;
    f.Name = id;
    f.Type = "string";
    f.Family = family;
    return f;
}

IssueDraft MakeBasicCreateDraft(const std::string& projectKey = "PROJ", const std::string& issueTypeId = "10001") {
    IssueDraft d;
    d.ProjectKey = projectKey;
    d.IssueTypeId = issueTypeId;
    d.IssueTypeName = "Story";
    d.FieldValues["summary"] = "Test summary";
    d.FieldValues["priority"] = "High";
    return d;
}

RequiredFieldSet EmptyRequired() {
    RequiredFieldSet r;
    r.RequiresIssueType = true;
    return r;
}

std::vector<TrackerField> BasicCatalog() {
    return {
        MakeField("summary"),
        MakeField("priority"),
        MakeField("status"),
        MakeField("description"),
    };
}

} // namespace

TEST_CASE("IssueCreatePipeline: Run create path posts payload, seeds cache, returns key") {
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", {{"summary", "Test summary"}}}});
    client.EnqueueCreateIssueSuccess("PROJ-42");

    FakeCacheFixture fix;
    const auto draft = MakeBasicCreateDraft();
    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());

    CHECK(result.Ok);
    CHECK(result.Error.empty());
    CHECK(result.IssueKey == "PROJ-42");
    CHECK(result.MissingFieldIds.empty());
    CHECK(client.CreateIssueCallCount() == 1);
    CHECK(client.BuildCreatePayloadCallCount() == 1);

    // Cache seeded with the new row + draft field values.
    CachedTicket cached;
    REQUIRE(fix.Ref().TryGetTicket("Jira", "PROJ-42", cached));
    CHECK(cached.fieldValues["summary"] == "Test summary");
    CHECK(cached.fieldValues["priority"] == "High");
}

TEST_CASE("IssueCreatePipeline: Run create path with null cache still returns Ok") {
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json::object());
    client.EnqueueCreateIssueSuccess("PROJ-1");

    auto result = IssueCreatePipeline::Run(client, nullptr, std::string(), MakeBasicCreateDraft(), EmptyRequired(),
                                           BasicCatalog());
    CHECK(result.Ok);
    CHECK(result.IssueKey == "PROJ-1");
    CHECK(result.SeededTicket.id == "PROJ-1");
}

TEST_CASE("IssueCreatePipeline: Run reports missing required field ids and does not call CreateIssue" *
          doctest::test_suite("[high-risk]")) {
    FakeTrackerClient client;
    FakeCacheFixture fix;

    IssueDraft draft;
    draft.ProjectKey = ""; // missing project
    draft.IssueTypeId = "";
    // FieldValues has no "summary"

    RequiredFieldSet required;
    required.FieldIds.insert("summary");
    required.RequiresIssueType = true;

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, required, BasicCatalog());
    CHECK_FALSE(result.Ok);
    CHECK(result.Error.find("Missing required") != std::string::npos);
    REQUIRE(result.MissingFieldIds.size() == 3);
    // MissingRequiredFields sorts its output; pin the exact triple.
    CHECK(result.MissingFieldIds[0] == "__issuetype__");
    CHECK(result.MissingFieldIds[1] == "__project__");
    CHECK(result.MissingFieldIds[2] == "summary");
    CHECK(client.CreateIssueCallCount() == 0);
    CHECK(client.BuildCreatePayloadCallCount() == 0);
}

TEST_CASE("IssueCreatePipeline: Run propagates CreateIssue failure as result.Error") {
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json::object());
    client.EnqueueCreateIssueFailure("HTTP 500: server unavailable");

    FakeCacheFixture fix;
    auto result =
        IssueCreatePipeline::Run(client, fix.Get(), "Jira", MakeBasicCreateDraft(), EmptyRequired(), BasicCatalog());
    CHECK_FALSE(result.Ok);
    CHECK(result.Error == "HTTP 500: server unavailable");
    CHECK(result.IssueKey.empty());
    CHECK(client.CreateIssueCallCount() == 1);
    // Nothing seeded in cache.
    CHECK(fix.Ref().GetAllTicketIds("Jira").empty());
}

TEST_CASE("IssueCreatePipeline: Run propagates BuildCreatePayload failure as result.Error") {
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(false, nlohmann::json::object(), "Unknown field 'foo'");

    FakeCacheFixture fix;
    auto result =
        IssueCreatePipeline::Run(client, fix.Get(), "Jira", MakeBasicCreateDraft(), EmptyRequired(), BasicCatalog());
    CHECK_FALSE(result.Ok);
    CHECK(result.Error == "Unknown field 'foo'");
    CHECK(client.CreateIssueCallCount() == 0);
}

TEST_CASE("IssueCreatePipeline: Run dispatches to update path when ExistingIssueKey is set") {
    FakeTrackerClient client;
    client.SetBuildUpdatePayloadResult(true, nlohmann::json{{"summary", "Updated"}});

    FakeCacheFixture fix;
    // Seed cache with existing ticket
    CachedTicket existing;
    existing.id = "PROJ-99";
    existing.fieldValues["summary"] = "Original";
    existing.fieldValues["priority"] = "Low";
    fix.Ref().SaveTicket("Jira", existing);

    IssueDraft draft = MakeBasicCreateDraft();
    draft.ExistingIssueKey = "PROJ-99";
    draft.FieldValues["summary"] = "Updated";

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());
    CHECK(result.Ok);
    CHECK(result.IssueKey == "PROJ-99");
    CHECK(client.CreateIssueCallCount() == 0); // NOT a create
    CHECK(client.UpdateIssueFieldsCallCount() == 1);
    CHECK(client.BuildUpdatePayloadCallCount() == 1);

    // Cache row should reflect the PUT — only "summary" was in the update payload, so
    // MergeDraftIntoCachedTicketForUpdate should overlay only that.
    CachedTicket cached;
    REQUIRE(fix.Ref().TryGetTicket("Jira", "PROJ-99", cached));
    CHECK(cached.fieldValues["summary"] == "Updated");
    CHECK(cached.fieldValues["priority"] == "Low"); // preserved from existing — not in put-fields
}

TEST_CASE("IssueCreatePipeline: Update path propagates UpdateIssueFields failure" *
          doctest::test_suite("[high-risk]")) {
    FakeTrackerClient client;
    client.SetBuildUpdatePayloadResult(true, nlohmann::json{{"summary", "Updated"}});
    client.EnqueueUpdateIssueFieldsFailure("HTTP 500: cannot reach issue server");

    FakeCacheFixture fix;
    CachedTicket existing;
    existing.id = "PROJ-50";
    existing.fieldValues["summary"] = "Original";
    fix.Ref().SaveTicket("Jira", existing);

    IssueDraft draft;
    draft.ExistingIssueKey = "PROJ-50";
    draft.FieldValues["summary"] = "Updated";

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());
    CHECK_FALSE(result.Ok);
    CHECK(result.Error == "HTTP 500: cannot reach issue server");
    CHECK(client.UpdateIssueFieldsCallCount() == 1);

    // Cache row must remain at original — failed PUT cannot have corrupted local state.
    CachedTicket cached;
    REQUIRE(fix.Ref().TryGetTicket("Jira", "PROJ-50", cached));
    CHECK(cached.fieldValues["summary"] == "Original");
}

TEST_CASE("IssueCreatePipeline: Update path with empty fields payload skips PUT but still ok") {
    // BuildUpdatePayload returning an empty object means "nothing to update" — pipeline must
    // not call UpdateIssueFields and should still flag Ok. Used by the "only attachments
    // changed" scenario in production.
    FakeTrackerClient client;
    client.SetBuildUpdatePayloadResult(true, nlohmann::json::object());

    FakeCacheFixture fix;
    CachedTicket existing;
    existing.id = "PROJ-101";
    existing.fieldValues["summary"] = "Same";
    fix.Ref().SaveTicket("Jira", existing);

    IssueDraft draft;
    draft.ExistingIssueKey = "PROJ-101";

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());
    CHECK(result.Ok);
    CHECK(result.IssueKey == "PROJ-101");
    CHECK(client.UpdateIssueFieldsCallCount() == 0);
}

TEST_CASE("IssueCreatePipeline: Create path with attachment failures returns Ok + failure list") {
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json::object());
    client.EnqueueCreateIssueSuccess("PROJ-7");
    client.SetAttachFilesResult(true, {{"/path/a.png", "io error"}, {"/path/b.txt", "denied"}});

    FakeCacheFixture fix;
    IssueDraft draft = MakeBasicCreateDraft();
    StagedAttachment a;
    a.AbsPath = "/path/a.png";
    a.FileName = "a.png";
    StagedAttachment b;
    b.AbsPath = "/path/b.txt";
    b.FileName = "b.txt";
    draft.StagedAttachments.push_back(a);
    draft.StagedAttachments.push_back(b);

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());
    CHECK(result.Ok);
    CHECK(result.IssueKey == "PROJ-7");
    CHECK(result.AttachmentFailures.size() == 2);
    CHECK(client.AttachFilesCallCount() == 1);
    CHECK(client.AttachFilesCalls()[0].Paths.size() == 2);
}

TEST_CASE("IssueCreatePipeline: create path applies status post-step and merges it into the cache") {
    // Exercises ApplyPostIssueStatusStep: a draft "status" triggers a follow-up UpdateIssueFields
    // call, and on success the status is merged back into the seeded cache row.
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json::object());
    client.EnqueueCreateIssueSuccess("PROJ-50");
    client.EnqueueUpdateIssueFieldsSuccess();

    FakeCacheFixture fix;
    IssueDraft draft = MakeBasicCreateDraft();
    draft.FieldValues["status"] = "Done";

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());
    CHECK(result.Ok);
    CHECK(result.IssueKey == "PROJ-50");
    // One UpdateIssueFields call for the status transition (create path makes no other PUT).
    CHECK(client.UpdateIssueFieldsCallCount() == 1);

    CachedTicket cached;
    REQUIRE(fix.Ref().TryGetTicket("Jira", "PROJ-50", cached));
    CHECK(cached.fieldValues["status"] == "Done");
}

TEST_CASE("IssueCreatePipeline: create path adds issue to each resolved sprint segment") {
    // Exercises ApplyPostIssueSprintSteps + ApplySprintFieldSegments: a comma-separated
    // sprint field resolves to two numeric ids and triggers two AddIssueToSprint calls.
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json::object());
    client.EnqueueCreateIssueSuccess("PROJ-60");
    client.SetDefaultAddIssueToSprintResult(true);

    std::vector<TrackerField> catalog = BasicCatalog();
    catalog.push_back(MakeField("sprint", TrackerFieldFamily::Sprint));

    FakeCacheFixture fix;
    IssueDraft draft = MakeBasicCreateDraft();
    draft.FieldValues["sprint"] = "101, 202";

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), catalog);
    CHECK(result.Ok);
    CHECK(result.IssueKey == "PROJ-60");
    REQUIRE(client.AddIssueToSprintCallCount() == 2);
    CHECK(client.AddIssueToSprintCalls()[0].SprintId == "101");
    CHECK(client.AddIssueToSprintCalls()[1].SprintId == "202");
}

TEST_CASE("IssueCreatePipeline: duplicate sprint ids are de-duplicated to a single AddIssueToSprint call") {
    // Guards the appliedSprintIds set inside ApplySprintFieldSegments.
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json::object());
    client.EnqueueCreateIssueSuccess("PROJ-61");
    client.SetDefaultAddIssueToSprintResult(true);

    std::vector<TrackerField> catalog = BasicCatalog();
    catalog.push_back(MakeField("sprint", TrackerFieldFamily::Sprint));

    FakeCacheFixture fix;
    IssueDraft draft = MakeBasicCreateDraft();
    draft.FieldValues["sprint"] = "303, 303";

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), catalog);
    CHECK(result.Ok);
    CHECK(client.AddIssueToSprintCallCount() == 1);
    CHECK(client.AddIssueToSprintCalls()[0].SprintId == "303");
}

TEST_CASE("IssueCreatePipeline: Run dispatches to update via legacy FieldValues[\"key\"] fallback") {
    // Legacy callers passed the existing-issue key via `FieldValues["key"]`; production code
    // still honors it as a fallback — guard against regression.
    FakeTrackerClient client;
    client.SetBuildUpdatePayloadResult(true, nlohmann::json{{"summary", "X"}});

    FakeCacheFixture fix;
    IssueDraft draft;
    draft.FieldValues["key"] = "PROJ-300";
    draft.FieldValues["summary"] = "X";

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());
    CHECK(result.Ok);
    CHECK(result.IssueKey == "PROJ-300");
    CHECK(client.UpdateIssueFieldsCallCount() == 1);
    CHECK(client.CreateIssueCallCount() == 0);
}

TEST_CASE("IssueCreatePipeline: empty ExistingIssueKey falls through to create even if 'key' field is whitespace") {
    FakeTrackerClient client;
    client.SetBuildCreatePayloadResult(true, nlohmann::json::object());
    client.EnqueueCreateIssueSuccess("PROJ-999");

    FakeCacheFixture fix;
    IssueDraft draft = MakeBasicCreateDraft();
    draft.FieldValues["key"] = "   "; // whitespace-only should not look like an existing key

    auto result = IssueCreatePipeline::Run(client, fix.Get(), "Jira", draft, EmptyRequired(), BasicCatalog());
    CHECK(result.Ok);
    CHECK(client.CreateIssueCallCount() == 1);
    CHECK(client.UpdateIssueFieldsCallCount() == 0);
}

// Slice 3 of the tracker Result<T> migration flipped the ITrackerIssueMutations payload
// builders from `bool(..., json& out, std::string& outError)` to
// `Result<nlohmann::json, TrackerError>`. These cases pin the migrated return shape directly:
// Ok carries the built json, Err carries a TrackerErrorInvalidRequest whose Detail is the
// verbatim builder message (the contract the AppController wrappers + IssueCreatePipeline
// translate back to their bool+outError / IssueCreateResult.Error surfaces).
TEST_CASE("BuildFieldPayload returns Ok json on success and InvalidRequest Err on failure") {
    FakeTrackerClient client;

    SUBCASE("ok carries the built payload") {
        client.SetBuildFieldPayloadResult(true);
        auto r = client.BuildFieldPayload(MakeField("summary"), {"hello"});
        REQUIRE(static_cast<bool>(r));
        CHECK(r.value().contains("values"));
        CHECK(r.value()["values"].size() == 1);
    }

    SUBCASE("err carries the verbatim detail under InvalidRequest") {
        client.SetBuildFieldPayloadResult(false, "Field 'summary': bad value.");
        auto r = client.BuildFieldPayload(MakeField("summary"), {"x"});
        REQUIRE_FALSE(static_cast<bool>(r));
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
        CHECK(r.error().Detail == "Field 'summary': bad value.");
    }
}

TEST_CASE("BuildCreatePayload returns Ok json on success and InvalidRequest Err on failure") {
    FakeTrackerClient client;

    SUBCASE("ok carries the built payload") {
        client.SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", nlohmann::json::object()}});
        auto r = client.BuildCreatePayload(MakeBasicCreateDraft(), BasicCatalog());
        REQUIRE(static_cast<bool>(r));
        CHECK(r.value().contains("fields"));
    }

    SUBCASE("err carries the verbatim detail under InvalidRequest") {
        client.SetBuildCreatePayloadResult(false, nlohmann::json::object(), "Project key is empty.");
        auto r = client.BuildCreatePayload(MakeBasicCreateDraft(), BasicCatalog());
        REQUIRE_FALSE(static_cast<bool>(r));
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
        CHECK(r.error().Detail == "Project key is empty.");
    }
}

// Slice 4 of the tracker Result<T> migration flipped the ITrackerIssueMutations void-payload
// writes from `bool(..., std::string& outError)` to a bare `TrackerError` return (no payload —
// ok-ness is carried by TrackerError::IsOk()). These cases pin the migrated shape directly:
// success returns an Ok TrackerError (empty Detail); a scripted failure returns a non-Ok
// TrackerError whose Detail is the verbatim scripted message — the contract the AppController
// wrappers + OfflineQueueService + IssueCreatePipeline translate back to their string surfaces
// (where ErrorTextContainsHttpStatus parses it verbatim).
TEST_CASE("UpdateIssueFields returns Ok TrackerError on success and a detail-carrying Err on failure") {
    FakeTrackerClient client;

    SUBCASE("success is an Ok TrackerError with empty detail") {
        client.EnqueueUpdateIssueFieldsSuccess();
        const TrackerError e = client.UpdateIssueFields("PROJ-1", nlohmann::json{{"summary", "x"}});
        CHECK(e.IsOk());
        CHECK(e.Detail.empty());
    }

    SUBCASE("failure carries the verbatim scripted detail") {
        client.EnqueueUpdateIssueFieldsFailure("HTTP 422: Unprocessable Entity");
        const TrackerError e = client.UpdateIssueFields("PROJ-1", nlohmann::json{{"summary", "x"}});
        CHECK_FALSE(e.IsOk());
        CHECK(e.Detail == "HTTP 422: Unprocessable Entity");
    }
}

TEST_CASE("UpdateField returns Ok TrackerError on success and a detail-carrying Err on failure") {
    FakeTrackerClient client;

    SUBCASE("success is an Ok TrackerError") {
        client.SetDefaultUpdateFieldResult(true);
        const TrackerError e = client.UpdateField("PROJ-1", MakeField("summary"), {"x"});
        CHECK(e.IsOk());
    }

    SUBCASE("failure carries the verbatim scripted detail") {
        client.SetDefaultUpdateFieldResult(false, "field rejected");
        const TrackerError e = client.UpdateField("PROJ-1", MakeField("summary"), {"x"});
        CHECK_FALSE(e.IsOk());
        CHECK(e.Detail == "field rejected");
    }
}

TEST_CASE("AddIssueToSprint returns Ok TrackerError on success and a detail-carrying Err on failure") {
    FakeTrackerClient client;

    SUBCASE("success is an Ok TrackerError") {
        client.EnqueueAddIssueToSprintSuccess();
        const TrackerError e = client.AddIssueToSprint("PROJ-1", "101");
        CHECK(e.IsOk());
    }

    SUBCASE("failure carries the verbatim scripted detail") {
        client.EnqueueAddIssueToSprintFailure("sprint not found");
        const TrackerError e = client.AddIssueToSprint("PROJ-1", "101");
        CHECK_FALSE(e.IsOk());
        CHECK(e.Detail == "sprint not found");
    }
}

// Slice 5 of the tracker Result<T> migration flipped the two payload-returning Mutations virtuals:
// CreateIssue (bool/string-out → Result<std::string, TrackerError>, Ok = new issue key) and
// AttachFilesToIssue (bool + outFailures-out → Result<vector<pair>, TrackerError>, where the Ok
// payload is the per-file failures list — partial success is NOT an error, plan landmine L3; Err
// is reserved for a hard failure that aborts the whole attach). These cases pin both shapes.
TEST_CASE("CreateIssue returns Ok key on success and an InvalidRequest Err on failure") {
    FakeTrackerClient client;

    SUBCASE("ok carries the new issue key") {
        client.EnqueueCreateIssueSuccess("PROJ-77");
        auto r = client.CreateIssue(nlohmann::json{{"summary", "x"}});
        REQUIRE(static_cast<bool>(r));
        CHECK(r.value() == "PROJ-77");
    }

    SUBCASE("err carries the verbatim detail under InvalidRequest") {
        client.EnqueueCreateIssueFailure("HTTP 400: bad fields");
        auto r = client.CreateIssue(nlohmann::json{{"summary", "x"}});
        REQUIRE_FALSE(static_cast<bool>(r));
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
        CHECK(r.error().Detail == "HTTP 400: bad fields");
    }
}

TEST_CASE("AttachFilesToIssue: per-file failures are an Ok payload (L3), hard failure is an Err") {
    FakeTrackerClient client;

    SUBCASE("full success is Ok with an empty failures list") {
        client.SetAttachFilesResult(true);
        auto r = client.AttachFilesToIssue("PROJ-1", {"/tmp/a.png"});
        REQUIRE(static_cast<bool>(r));
        CHECK(r.value().empty());
    }

    SUBCASE("partial success is Ok carrying the per-file failures (NOT an Err)") {
        client.SetAttachFilesResult(true, {{"/tmp/b.png", "HTTP 413: too large"}});
        auto r = client.AttachFilesToIssue("PROJ-1", {"/tmp/b.png"});
        REQUIRE(static_cast<bool>(r));
        REQUIRE(r.value().size() == 1);
        CHECK(r.value()[0].first == "/tmp/b.png");
        CHECK(r.value()[0].second == "HTTP 413: too large");
    }

    SUBCASE("a hard failure is an Err carrying the detail") {
        client.SetAttachFilesResult(false, {}, "Issue key is empty.");
        auto r = client.AttachFilesToIssue("", {"/tmp/c.png"});
        REQUIRE_FALSE(static_cast<bool>(r));
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
        CHECK(r.error().Detail == "Issue key is empty.");
    }
}

// Slice 6 of the tracker Result<T> migration flipped the Reader virtual FetchIssuesForKeys off
// bool(..., vector<CachedTicket>& out, std::string& outError) to Result<vector<CachedTicket>,
// TrackerError> (Ok = the fetched tickets). These cases pin the migrated shape via the fake's
// unchanged SetFetchIssuesForKeysResult scripting API (the callers — AppController prefetch +
// OfflineQueueService conflict re-fetch — branch on the Result's structured kind since N12 item 13,
// with .Detail preserved verbatim for messages/audit).
TEST_CASE("FetchIssuesForKeys returns Ok tickets on success and a detail-carrying Err on failure") {
    FakeTrackerClient client;
    ViewsStore views;

    SUBCASE("ok carries the scripted tickets") {
        CachedTicket t;
        t.id = "PROJ-7";
        client.SetFetchIssuesForKeysResult(true, {t});
        auto r = client.FetchIssuesForKeys(TrackerConfig{}, {"PROJ-7"}, views);
        REQUIRE(static_cast<bool>(r));
        REQUIRE(r.value().size() == 1);
        CHECK(r.value()[0].id == "PROJ-7");
    }

    SUBCASE("failure carries the verbatim scripted detail (transport text preserved)") {
        client.SetFetchIssuesForKeysResult(false, {}, "Operation timed out after 30000 ms");
        auto r = client.FetchIssuesForKeys(TrackerConfig{}, {"PROJ-7"}, views);
        REQUIRE_FALSE(static_cast<bool>(r));
        CHECK(r.error().Detail == "Operation timed out after 30000 ms");
    }
}

// Slice 7 of the tracker Result<T> migration flipped the ITrackerCollaboration virtuals: the reads
// (FetchIssueWatchers / FetchIssueVotes / SearchUsersByQuery / FetchIssueComments — FetchUserGroupNames
// has since moved to ITrackerActivity) to Result<payload, TrackerError>, and the writes (AddIssueWatcher /
// AddIssueCommentPlain / AddWorklog / AddIssueCommentAnnotateContext) to a bare TrackerError. FetchIssueVotes' four
// out-params (voters + int* + bool* + bool*) collapsed into the TrackerIssueVotes Ok payload. These cases pin both the
// new interface defaults (unsupported → Err) and the migrated value shapes via a minimal stub override.
namespace {
class StubCollaboration : public ITrackerCollaboration {
  public:
    // Override two methods to pin the Ok shapes; leave the rest as interface defaults (→ Err).
    Result<TrackerIssueVotes, TrackerError> FetchIssueVotes(const TrackerConfig& /*cfg*/,
                                                            const std::string& /*issueKey*/) override {
        TrackerIssueVotes v;
        v.VoteCount = 3;
        v.HasVoted = true;
        v.VotersArrayInResponse = true;
        TrackerUser u;
        u.AccountId = "acc-1";
        v.Voters.push_back(u);
        return Result<TrackerIssueVotes, TrackerError>::Ok(std::move(v));
    }
    TrackerError AddIssueWatcher(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) override {
        return TrackerError::Ok();
    }
};
} // namespace

TEST_CASE("ITrackerCollaboration migrated shapes: Result reads, bare-TrackerError writes, votes struct") {
    StubCollaboration collab;

    SUBCASE("an overridden read returns its payload as the Ok value (votes struct collapses 4 out-params)") {
        auto r = collab.FetchIssueVotes(TrackerConfig{}, "PROJ-1");
        REQUIRE(static_cast<bool>(r));
        CHECK(r.value().VoteCount == 3);
        CHECK(r.value().HasVoted);
        CHECK(r.value().VotersArrayInResponse);
        REQUIRE(r.value().Voters.size() == 1);
        CHECK(r.value().Voters[0].AccountId == "acc-1");
    }

    SUBCASE("an overridden write returns an Ok TrackerError") {
        const TrackerError e = collab.AddIssueWatcher(TrackerConfig{}, "PROJ-1");
        CHECK(e.IsOk());
    }

    SUBCASE("a non-overridden read default is an InvalidRequest Err (Result<vector<TrackerUser>>)") {
        auto r = collab.SearchUsersByQuery(TrackerConfig{}, "ali");
        REQUIRE_FALSE(static_cast<bool>(r));
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
    }

    SUBCASE("a non-overridden write default is an InvalidRequest Err (bare TrackerError)") {
        const TrackerError e = collab.AddWorklog(TrackerConfig{}, "PROJ-1", "1h", "", "", "", "");
        CHECK_FALSE(e.IsOk());
        CHECK(e.Kind == TrackerErrorKind::InvalidRequest);
    }
}
