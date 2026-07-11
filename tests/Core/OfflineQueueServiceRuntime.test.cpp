// OfflineQueueService runtime tests — drive the service through `IOfflineQueueDeps` against
// a `FakeOfflineQueueDeps` + `FakeTrackerClient` + the in-memory `FakeSyncCache` (ADR-0020 —
// contract-suite-verified against the real cache; no SQLite). Every
// case constructs a fresh fixture stack so queue / cache / mock state never leaks across
// `TEST_CASE`s. Each test owns a `OfflineQueueTestEnvGuard` that redirects the audit writer + the
// ConfigManager user-data dir into a private temp dir, and writes a minimal config file
// with `read_only_mode=false` (ConfigManager defaults that to true when no config exists).
//
// `OfflineQueueService.cpp` calls `IsTrackerTransportErrorText`, now defined in the cpr-free
// `TrackerHttpPure.cpp` (#1339 moved it off cpr so Sync + the TSan threading subset link it
// without dragging cpr — backlog C12 closed). Both the full test target and the TSan subset
// link that TU, so the production definition resolves directly — no test-side mirror needed.

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "OfflineQueueReplayPolicy.h"
#include "OfflineQueueService.h"
#include "TrackerHttpPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

using smatchet_tests::FakeOfflineQueueDeps;
// Per-test temp dir + ConfigManager redirection + read_only_mode=false config. Extracted to
// tests/support/OfflineQueueTestEnv.h (multi-grid Slice 1c) so the backend-key replay TU
// reuses it; semantics unchanged.
using smatchet_tests::OfflineQueueTestEnvGuard;

namespace {

// Wait for the async audit writer to flush at least `expectedMin` lines that include
// `needle` to the current audit file path. Returns the number observed.
std::size_t WaitForAuditLines(const std::string& needle, std::size_t expectedMin, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::size_t matches = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream f(BackendAuditTrail::GetAuditFilePath(), std::ios::binary);
        if (f.is_open()) {
            matches = 0;
            std::string line;
            while (std::getline(f, line)) {
                if (line.find(needle) != std::string::npos)
                    ++matches;
            }
            if (matches >= expectedMin)
                return matches;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return matches;
}

TrackerField MakeField(const std::string& id, TrackerFieldFamily family = TrackerFieldFamily::Text) {
    TrackerField f;
    f.Id = id;
    f.Name = id;
    f.Type = "string";
    f.Family = family;
    return f;
}

IssueDraft MakeBasicCreateDraft(const std::string& summary = "Test summary") {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.IssueTypeName = "Story";
    d.FieldValues["summary"] = summary;
    return d;
}

// Configure a deps fixture so its FakeTrackerClient successfully serves `IssueCreatePipeline::Run`:
// BuildCreatePayload returns a non-empty object; CreateIssue defaults to scripted queue.
void PrimeCreatePipelineHappy(FakeOfflineQueueDeps& deps) {
    deps.BackendImpl->SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", {{"summary", "X"}}}});
    deps.Fields = {MakeField("summary"), MakeField("priority")};
}

// Deps variant whose MutationsShared() role can be toggled null at runtime, leaving
// Cache()/ReaderShared() valid — exercises the `if (!reader || !mutations)` early-return
// inside TickOfflineFieldEdits (both other roles must stay non-null to reach that guard).
// #16 regression fixture; latched-handle form since the debt 2026-06-07 fix.
class NullableMutationsDeps : public FakeOfflineQueueDeps {
  public:
    bool MutationsNull = false;
    std::shared_ptr<ITrackerIssueMutations> MutationsShared() const override {
        return MutationsNull ? nullptr : FakeOfflineQueueDeps::MutationsShared();
    }
};

// Deps whose BuildFieldPayload mirrors the PRODUCTION shape for a STRUCTURED field:
// `{ field.Id : { "id": <value> } }` — exactly what JiraClient::BuildFieldPayload emits
// (`{ field.Id : BuildValue(...) }`, where BuildValue for a select/priority/status field
// returns `{"id":...}`; see Source/Core/src/Tracker/JiraIssueMutation.cpp BuildFieldPayload +
// TrackerFieldPayloadPure::BuildFieldOptionPayload). #854 regression fixture: proves the
// scalar conflict-resolve path routes through this builder seam instead of clobbering the
// queued object payload with a bare display string.
class StructuredFieldBackend : public smatchet_tests::FakeTrackerClient {
  public:
    Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                           const std::vector<std::string>& values) override {
        if (values.empty()) {
            return Result<nlohmann::json, TrackerError>::Ok(nlohmann::json::object({{field.Id, nullptr}}));
        }
        // Structured single-select shape: `{ fieldId : { "id": value } }`.
        nlohmann::json inner = nlohmann::json::object();
        inner["id"] = values.front();
        return Result<nlohmann::json, TrackerError>::Ok(nlohmann::json::object({{field.Id, std::move(inner)}}));
    }
};

class StructuredFieldDeps : public FakeOfflineQueueDeps {
  public:
    std::shared_ptr<StructuredFieldBackend> Structured{std::make_shared<StructuredFieldBackend>()};
    std::shared_ptr<ITrackerIssueMutations> MutationsShared() const override { return Structured; }
};

} // namespace

// ---------------------------------------------------------------------------
// Case 1 — Enqueue create-issue → drain 200 OK → row deleted from queue. [high-risk]
// Mutation-sanity (test-side): flipping the expected `Pending=0` to `Pending=1` fails the
// assertion, confirming the post-success delete is what flips the row off the queue.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: create → drain 200 OK deletes row" * doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-42");

    OfflineQueueService svc(deps);
    const auto id = svc.QueueCreateOffline(MakeBasicCreateDraft());
    REQUIRE(id > 0);
    CHECK(svc.GetPendingCreateCount() == 1u);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    CHECK(svc.GetPendingCreateCount() == 0u);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
    CHECK(deps.BackendImpl->CreateIssueCallCount() == 1u);
    CHECK(deps.RefreshLocalDataCalls == 1);
    CHECK(deps.DeferredLiveNotifyCalls == 1);
}

// ---------------------------------------------------------------------------
// Case 2 — Enqueue create-issue → drain 4xx → row stays in queue (4xx alone does NOT
// dead-letter; only the cap does for the create path). [high-risk]
// The case description in the plan says "4xx archives". After implementation review of
// `TickOfflineCreates`, the create path never branches on `IsTrackerTransportErrorText` —
// only `ShouldArchive(nextAttempts)` decides archive-vs-retry. So a 4xx with attempts < cap
// keeps the row, increments attempts, populates LastError. Documented as a plan deviation
// in `docs/plans/active/applied/test-suite-expansion.md`.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: create → drain 4xx increments attempts, no dead-letter" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueFailure("HTTP 422: validation failed");

    OfflineQueueService svc(deps);
    const auto id = svc.QueueCreateOffline(MakeBasicCreateDraft());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    REQUIRE(svc.GetPendingCreateCount() == 1u);
    const auto rows = svc.GetPendingCreates();
    CHECK(rows.front().Attempts == 1);
    CHECK(rows.front().LastError.find("422") != std::string::npos);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
}

// ---------------------------------------------------------------------------
// Case 3 — Enqueue create-issue → 5xx (server error, transport-classified) → stays + attempts++.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: create → drain 5xx increments attempts, stays in queue") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueFailure("HTTP 503: service unavailable");

    OfflineQueueService svc(deps);
    REQUIRE(svc.QueueCreateOffline(MakeBasicCreateDraft()) > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    REQUIRE(svc.GetPendingCreateCount() == 1u);
    CHECK(svc.GetPendingCreates().front().Attempts == 1);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
}

// ---------------------------------------------------------------------------
// Case 4 — Enqueue create-issue → timeout → stays + attempts++.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: create → drain timeout increments attempts, stays in queue") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueFailure("Operation timed out after 30000 ms");

    OfflineQueueService svc(deps);
    REQUIRE(svc.QueueCreateOffline(MakeBasicCreateDraft()) > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    REQUIRE(svc.GetPendingCreateCount() == 1u);
    CHECK(svc.GetPendingCreates().front().Attempts == 1);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
}

// ---------------------------------------------------------------------------
// Case 5 — Enqueue create-issue → 401 (auth) → row stays in queue, no dead-letter.
// Phase-3 hostile-fixture target: the create path must NOT collapse an auth error into
// dead-letter while attempts << cap. [high-risk]
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: create → 401 stays in queue, no dead-letter on auth" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueFailure("HTTP 401: invalid credentials");

    OfflineQueueService svc(deps);
    REQUIRE(svc.QueueCreateOffline(MakeBasicCreateDraft()) > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    REQUIRE(svc.GetPendingCreateCount() == 1u);
    CHECK(svc.GetPendingCreates().front().Attempts == 1);
    CHECK(svc.GetPendingCreates().front().LastError.find("401") != std::string::npos);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
}

// ---------------------------------------------------------------------------
// Case 6 — Repeated failures → row dead-lettered exactly at the
// `OfflineQueueReplayPolicy::kMaxReplayAttempts` boundary.
//
// Setup: pre-load attempts = kMaxReplayAttempts - 1 (boundary minus one). One more failed
// tick takes nextAttempts = kMaxReplayAttempts → `ShouldArchive(nextAttempts)` returns
// true → row archives.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: create → repeated failure archives at kMaxReplayAttempts boundary") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->SetDefaultCreateIssueResult(false, "HTTP 503: still down");

    OfflineQueueService svc(deps);
    const auto id = svc.QueueCreateOffline(MakeBasicCreateDraft());
    REQUIRE(id > 0);
    // Walk the row to one-below-cap so the next tick tips it over.
    const int preload = OfflineQueueReplayPolicy::kMaxReplayAttempts - 1;
    deps.CacheImpl->UpdatePendingCreate(id, preload, "scripted");
    CHECK(svc.GetPendingCreates().front().Attempts == preload);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    CHECK(svc.GetPendingCreateCount() == 0u);
    REQUIRE(svc.GetDeadPendingCreateCount() == 1u);
    const auto dead = svc.GetDeadPendingCreates();
    CHECK(dead.front().Attempts == OfflineQueueReplayPolicy::kMaxReplayAttempts);
    CHECK(dead.front().TerminalReason == "max_attempts");
}

// ---------------------------------------------------------------------------
// Case 6b — Pre-attempt cap gate (post-restart): a row that already sits at the cap from a
// previous session must dead-letter on the NEXT tick without calling CreateIssue. Covers
// `OfflineQueueReplayPolicy::ShouldArchive` pre-attempt boundary (the cap check at the very
// top of the replay loop, before the create call).
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: create → already-at-cap row dead-letters without invoking backend") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    // Make the backend explode if it gets called — proves we never reached CreateIssue.
    deps.BackendImpl->SetDefaultCreateIssueResult(false, "should-not-be-called");

    OfflineQueueService svc(deps);
    const auto id = svc.QueueCreateOffline(MakeBasicCreateDraft());
    REQUIRE(id > 0);
    deps.CacheImpl->UpdatePendingCreate(id, OfflineQueueReplayPolicy::kMaxReplayAttempts, "from prior session");

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    CHECK(svc.GetPendingCreateCount() == 0u);
    CHECK(svc.GetDeadPendingCreateCount() == 1u);
    // Pre-attempt gate: backend was never touched.
    CHECK(deps.BackendImpl->CreateIssueCallCount() == 0u);
}

// ---------------------------------------------------------------------------
// Case 7 — Enqueue field-edit → drain 200 OK → row deleted.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: field-edit → drain 200 OK deletes row") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    OfflineQueueService svc(deps);
    std::string err;
    const nlohmann::json payload = nlohmann::json{{"summary", "Updated value"}};
    const auto id = svc.QueueFieldEditOffline("PROJ-1", "summary", payload.dump(), err, std::string());
    REQUIRE(err.empty());
    REQUIRE(id > 0);
    CHECK(svc.GetPendingFieldEdits().size() == 1u);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    CHECK(svc.GetPendingFieldEdits().empty());
    CHECK(svc.GetDeadPendingFieldEdits().empty());
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u);
    CHECK(deps.RefreshLocalDataCalls == 1);
    CHECK(deps.DeferredLiveNotifyCalls == 1);
}

// ---------------------------------------------------------------------------
// Case 8 — Enqueue field-edit → drain 4xx (hard rejection) → row archives to dead-letter.
// Unlike the create path, `TickOfflineFieldEdits` does branch on
// `IsTrackerTransportErrorText`: non-transport errors archive immediately as `replay_rejected`.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: field-edit → drain 4xx archives to dead-letter") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    deps.BackendImpl->EnqueueUpdateIssueFieldsFailure("HTTP 422: Unprocessable Entity");

    OfflineQueueService svc(deps);
    std::string err;
    const auto id = svc.QueueFieldEditOffline("PROJ-2", "summary", nlohmann::json{{"summary", "Bad value"}}.dump(), err,
                                              std::string());
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    CHECK(svc.GetPendingFieldEdits().empty());
    REQUIRE(svc.GetDeadPendingFieldEdits().size() == 1u);
    const auto dead = svc.GetDeadPendingFieldEdits().front();
    CHECK(dead.TerminalReason == "replay_rejected");
    CHECK(dead.OriginalId == id);
    CHECK(dead.IssueKey == "PROJ-2");
    CHECK(dead.FieldId == "summary");
}

// ---------------------------------------------------------------------------
// Case 9 — Audit-trail row emitted per replay attempt. Uses `BackendAuditTrail::ReadRecentEvents`
// filtered by the pending-create id encoded in the event's `operation_id` field.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: replay emits audit-trail row per attempt") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-44");

    OfflineQueueService svc(deps);
    const auto id = svc.QueueCreateOffline(MakeBasicCreateDraft());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    // Wait for the async audit writer to flush. The replay-create event's operation_id is
    // the stringified pending_create id; the queue-create event uses the same id. Both
    // should appear.
    const std::string idStr = std::to_string(id);
    const std::size_t lineMatches = WaitForAuditLines(idStr, 2u);
    CHECK(lineMatches >= 2u);

    std::string readErr;
    const auto events = BackendAuditTrail::ReadRecentEvents(500u, &readErr);
    CHECK(readErr.empty());
    std::size_t queueCount = 0;
    std::size_t replayCount = 0;
    for (const auto& ev : events) {
        if (ev.value("operation_id", std::string()) != idStr)
            continue;
        const std::string action = ev.value("action", std::string());
        if (action == "offline_queue_create")
            ++queueCount;
        else if (action == "offline_replay_create")
            ++replayCount;
    }
    CHECK(queueCount >= 1u);
    CHECK(replayCount >= 1u);
}

// ---------------------------------------------------------------------------
// Case 10 — Two sequential pending creates both drain in a single tick. Each gets its own
// audit row and the FakeTrackerClient sees CreateIssue twice in stable order. This replaces
// the plan's chained pending-create → field-edit case: production OfflineQueueService does
// not currently rewrite a queued field-edit's IssueKey when its referenced create succeeds
// (no such code path in `TickOfflineCreates`); the plan-intent for `chained replay` was
// "successive rows are processed independently". Documented in plan Deviations.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: two sequential creates both drain in one tick") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    PrimeCreatePipelineHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-100");
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-101");

    OfflineQueueService svc(deps);
    REQUIRE(svc.QueueCreateOffline(MakeBasicCreateDraft("First")) > 0);
    REQUIRE(svc.QueueCreateOffline(MakeBasicCreateDraft("Second")) > 0);
    REQUIRE(svc.GetPendingCreateCount() == 2u);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    CHECK(svc.GetPendingCreateCount() == 0u);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
    CHECK(deps.BackendImpl->CreateIssueCallCount() == 2u);
    // Both successes coalesce into a single RefreshLocalData (called once after the batch).
    CHECK(deps.RefreshLocalDataCalls == 1);
    CHECK(deps.DeferredLiveNotifyCalls == 2);
}

// ---------------------------------------------------------------------------
// Case 11 — Field-edit with merge conflict: drain fetches a server document whose rich value
// differs from the queued `OriginalRichValue` and from the local edit on overlapping lines.
// The 3-way merge surfaces the conflict, row is flagged with `HasMergeConflict=true`, and
// no UpdateIssueFields call is issued. [high-risk]
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: field-edit → merge conflict marks row, no PUT, no dead-letter" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    // Stage a server document with a `summary` field that diverged from base.
    CachedTicket fresh;
    fresh.id = "PROJ-30";
    fresh.fieldValues["summary"] = "Server moved on";
    fresh.fieldRichValues["summary"] = "Server moved on"; // plain-text rich (no '{', no '<') → toMd identity
    deps.BackendImpl->SetFetchIssuesForKeysResult(true, {fresh});

    OfflineQueueService svc(deps);
    std::string err;
    // Local edit: differs from base.
    const nlohmann::json payload = nlohmann::json{{"summary", "My local change"}};
    const std::string originalRich = "Common ancestor"; // base — differs from both mine and theirs
    const auto id = svc.QueueFieldEditOffline("PROJ-30", "summary", payload.dump(), err, originalRich);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    // Conflict path: row stays in active queue, no PUT issued, no dead-letter, conflict flag set.
    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    CHECK(svc.GetPendingFieldEdits().front().HasMergeConflict);
    CHECK(svc.GetDeadPendingFieldEdits().empty());
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 0u);
}

// ---------------------------------------------------------------------------
// ADR-0016 — Scalar conflict: server moved since the user edited → suspend (kind:scalar), no PUT.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: scalar conflict suspends with kind:scalar, no PUT" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    // Server's current display value for `status` diverged from the captured base.
    CachedTicket fresh;
    fresh.id = "PROJ-50";
    fresh.fieldValues["status"] = "Done"; // theirs
    deps.BackendImpl->SetFetchIssuesForKeysResult(true, {fresh});

    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"status", "In Review"}}.dump();
    // base="In Progress" (display), no rich base → scalar path. hasOriginalValue=true marks a
    // captured scalar base (ADR-0016 presence flag).
    const auto id = svc.QueueFieldEditOffline("PROJ-50", "status", payload, err, std::string(), "In Progress", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    const auto row = svc.GetPendingFieldEdits().front();
    CHECK(row.HasMergeConflict);
    CHECK(row.ConflictContextJson.find("\"kind\":\"scalar\"") != std::string::npos);
    CHECK(svc.GetDeadPendingFieldEdits().empty());
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 0u);
}

// ---------------------------------------------------------------------------
// ADR-0016 — Scalar, server unchanged: base == theirs → replay normally (no conflict).
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: scalar unchanged replays (no conflict)") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();
    CachedTicket fresh;
    fresh.id = "PROJ-51";
    fresh.fieldValues["priority"] = "High"; // theirs == base → no divergence
    deps.BackendImpl->SetFetchIssuesForKeysResult(true, {fresh});

    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"priority", "High"}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-51", "priority", payload, err, std::string(), "High", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    CHECK(svc.GetPendingFieldEdits().empty());
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u);
}

// ---------------------------------------------------------------------------
// ADR-0016 decision (c) — permanent re-fetch failure with a base captured → kind:unverified
// suspend (never silent dead-letter, never silent overwrite).
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: permanent re-fetch failure with base → unverified suspend" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    // 404 is permanent (IsTrackerTransportErrorText==false).
    deps.BackendImpl->SetFetchIssuesForKeysResult(false, {}, "HTTP 404: not found");

    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"status", "Done"}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-52", "status", payload, err, std::string(), "In Progress", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    const auto row = svc.GetPendingFieldEdits().front();
    CHECK(row.HasMergeConflict);
    CHECK(row.ConflictContextJson.find("\"kind\":\"unverified\"") != std::string::npos);
    CHECK(svc.GetDeadPendingFieldEdits().empty());
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 0u);
}

// ---------------------------------------------------------------------------
// ADR-0016 decision (c) — transient re-fetch failure retries (bumps attempts), then routes to
// unverified at the attempt cap. Never silently dead-lettered.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: transient re-fetch failure retries then unverified at cap" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    // Timeout is transient (IsTrackerTransportErrorText==true).
    deps.BackendImpl->SetFetchIssuesForKeysResult(false, {}, "Operation timed out after 30000 ms");

    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"status", "Done"}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-53", "status", payload, err, std::string(), "In Progress", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    // First tick: transient + below cap → retry (attempts bumped, no conflict yet).
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();
    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    CHECK_FALSE(svc.GetPendingFieldEdits().front().HasMergeConflict);
    CHECK(svc.GetPendingFieldEdits().front().Attempts == 1);
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 0u);

    // Walk attempts to one-below-cap; the next transient tick would tip it over → unverified.
    deps.CacheImpl->UpdatePendingFieldEdit(id, OfflineQueueReplayPolicy::kMaxReplayAttempts - 1, "transient");
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    const auto row = svc.GetPendingFieldEdits().front();
    CHECK(row.HasMergeConflict);
    CHECK(row.ConflictContextJson.find("\"kind\":\"unverified\"") != std::string::npos);
    CHECK(svc.GetDeadPendingFieldEdits().empty());
}

// ---------------------------------------------------------------------------
// ADR-0016 residue — a row with NO base captured (legacy) still replays (last-write-wins). The
// re-fetch is not even attempted (no base to compare against).
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: no-base row replays (documented last-write-wins residue)") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();
    // Even if a fetch WOULD diverge, no base means we never look.
    deps.BackendImpl->SetFetchIssuesForKeysResult(false, {}, "HTTP 404: not found");

    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"status", "Done"}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-54", "status", payload, err, std::string(), std::string());
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    CHECK(svc.GetPendingFieldEdits().empty());
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u);
    CHECK(deps.BackendImpl->FetchIssuesForKeysCallCount() == 0u);
}

// ---------------------------------------------------------------------------
// ADR-0016 (#3 regression) — a CAPTURED-but-BLANK scalar base (originalValue=="" with
// hasOriginalValue=true) is still conflict-checked: when the server moved from blank to a value,
// the replay suspends (kind:scalar), NOT silently last-write-wins. Detection keys on the presence
// flag, not emptiness.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: blank-but-captured scalar base still conflict-checks" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    // Server now has a non-empty value; the captured base was blank → server moved.
    CachedTicket fresh;
    fresh.id = "PROJ-57";
    fresh.fieldValues["status"] = "Done"; // theirs (base was "")
    deps.BackendImpl->SetFetchIssuesForKeysResult(true, {fresh});

    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"status", "In Review"}}.dump();
    // base="" but hasOriginalValue=true → presence, not emptiness, drives detection.
    const auto id = svc.QueueFieldEditOffline("PROJ-57", "status", payload, err, std::string(), std::string(), true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    // Without the presence flag this would have last-write-won (PUT issued, queue drained). With
    // it, the blank base is recognized and the conflict suspends.
    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    const auto row = svc.GetPendingFieldEdits().front();
    CHECK(row.HasMergeConflict);
    CHECK(row.ConflictContextJson.find("\"kind\":\"scalar\"") != std::string::npos);
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 0u);
}

// ---------------------------------------------------------------------------
// ADR-0016 (#1 regression) — resolving a conflict for a NON-EXISTENT row must NOT fabricate a
// payload. The service logs-and-skips; no fabricated `__resolved__`-keyed row is created.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: resolve on missing row does not fabricate a payload") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    OfflineQueueService svc(deps);

    // No row with id 999999 exists. Resolve must be a no-op (log-and-skip), not a fabricated write.
    svc.ResolveFieldEditConflict(999999, "whatever", std::string(), "scalar");

    CHECK(svc.GetPendingFieldEdits().empty());
    CHECK(svc.GetDeadPendingFieldEdits().empty());
}

// ---------------------------------------------------------------------------
// ADR-0016 — Discard resolution: hard-deletes the queue row AND emits an audit entry.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: discard hard-deletes row + emits audit entry") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"status", "Done"}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-55", "status", payload, err, std::string(), "In Progress", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);
    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);

    // The UI "Discard my edit" path routes through DeletePendingFieldEdits (ADR-0016).
    const auto summary = svc.DeletePendingFieldEdits({id});
    CHECK(summary.Deleted == 1);
    CHECK(svc.GetPendingFieldEdits().empty());
    CHECK(svc.GetDeadPendingFieldEdits().empty()); // dead-letter stays failures-only.

    // Audit entry for the discard (offline_queue_field_edit_delete) lands in the audit log.
    const std::size_t matches = WaitForAuditLines("offline_queue_field_edit_delete", 1u);
    CHECK(matches >= 1u);
}

// ---------------------------------------------------------------------------
// ADR-0016 — Resolve clears BOTH bases so the next replay performs the consented overwrite.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: resolve clears both rich + scalar bases") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"status", "Done"}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-56", "status", payload, err, std::string(), "In Progress", true);
    REQUIRE(id > 0);
    REQUIRE(svc.GetPendingFieldEdits().front().OriginalValue == "In Progress");

    // Resolve a scalar conflict to "Done" — clears bases, rewrites payload value.
    svc.ResolveFieldEditConflict(id, "Done", std::string(), "scalar");

    const auto row = svc.GetPendingFieldEdits().front();
    CHECK(row.OriginalValue.empty());     // scalar base nulled
    CHECK(row.OriginalRichValue.empty()); // rich base nulled
    CHECK_FALSE(row.HasMergeConflict);
}

// ---------------------------------------------------------------------------
// Case 12 — `OfflineQueueReplayPolicy::ShouldArchive` boundary coverage in isolation, both
// pre-attempt and post-failure shapes. Mirrors how the runtime invokes the policy.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: ShouldArchive boundary — pre-attempt + post-failure") {
    const int cap = OfflineQueueReplayPolicy::kMaxReplayAttempts;
    // Pre-attempt gate shape: a row with attempts == cap must archive without replay.
    CHECK_FALSE(OfflineQueueReplayPolicy::ShouldArchive(cap - 1));
    CHECK(OfflineQueueReplayPolicy::ShouldArchive(cap));
    CHECK(OfflineQueueReplayPolicy::ShouldArchive(cap + 1));
    // Post-failure gate shape: attempts = cap - 1, nextAttempts = cap → archive.
    const int nextAttempts = (cap - 1) + 1;
    CHECK(OfflineQueueReplayPolicy::ShouldArchive(nextAttempts));
    // Below cap → retry.
    CHECK_FALSE(OfflineQueueReplayPolicy::ShouldArchive(0));
    CHECK_FALSE(OfflineQueueReplayPolicy::ShouldArchive(1));
}

// ---------------------------------------------------------------------------
// #16 (build-quality-velocity-hardening) — the null-Mutations early-return inside
// TickOfflineFieldEdits must release the in-flight latch, exactly like the three sibling
// early-returns above it. Discriminating shape: tick once with Mutations()==null + a non-empty
// queue (the previously unguarded path), then restore Mutations() and tick again. WITH the fix
// the second tick replays + drains the row; WITHOUT it the latch is stuck true so the second
// tick short-circuits at the in-flight guard → UpdateIssueFields never runs, the edit is lost
// forever (the permanent wedge). [high-risk]
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: null Mutations early-return resets in-flight latch (#16)" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    NullableMutationsDeps deps;
    deps.MutationsNull = true; // live mutations role transiently unavailable

    OfflineQueueService svc(deps);
    std::string err;
    const auto id =
        svc.QueueFieldEditOffline("PROJ-16", "summary", nlohmann::json{{"summary", "v"}}.dump(), err, std::string());
    REQUIRE(err.empty());
    REQUIRE(id > 0);
    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);

    // First tick hits the null-Mutations early-return: row untouched, backend never called.
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();
    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 0u);

    // Mutations comes back online; a later tick MUST proceed (latch released by the fix).
    deps.MutationsNull = false;
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    CHECK(svc.GetPendingFieldEdits().empty());
    CHECK(svc.GetDeadPendingFieldEdits().empty());
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u);
}

// ---------------------------------------------------------------------------
// #854 (DATA-LOSS) — resolving a SCALAR conflict on a STRUCTURED field (priority/status/
// select, whose queued payload is `{"priority":{"id":"3"}}`) must rebuild the payload via the
// PRODUCTION builder (`mutations->BuildFieldPayload`), NOT clobber it with a bare display
// string. The old code wrote `{"priority":"Low"}` verbatim → Jira HTTP 400 → retries →
// dead-letter → the user's resolved offline edit silently lost. Here the structured-shape
// backend returns `{ fieldId : { "id": value } }` (the JiraClient::BuildFieldPayload shape),
// so the resolved payload must keep that object shape, not the flattened label.
//
// "Use Theirs"/"Save" direction — adopt the chosen value: the rebuilt payload is `{"id":...}`.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: #854 scalar resolve on structured field rebuilds object payload (theirs)" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    StructuredFieldDeps deps;
    deps.Fields = {MakeField("priority", TrackerFieldFamily::SelectSingle)};

    OfflineQueueService svc(deps);
    std::string err;
    // Queued payload is already the structured object shape (live edit path output).
    const std::string payload = nlohmann::json{{"priority", {{"id", "3"}}}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-854", "priority", payload, err, std::string(), "Low", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    // Resolve to "High" (the server's display label / "Use Theirs"). The service feeds the
    // display label into the production builder, which yields `{"id":"High"}` — NOT a bare
    // string.
    svc.ResolveFieldEditConflict(id, "High", std::string(), "scalar");

    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    const auto row = svc.GetPendingFieldEdits().front();
    const nlohmann::json resolved = nlohmann::json::parse(row.FieldsPayloadJson);
    REQUIRE(resolved.contains("priority"));
    // The whole bug: the value must stay an OBJECT, not be flattened to a display string.
    CHECK(resolved["priority"].is_object());
    CHECK(resolved["priority"].value("id", std::string()) == "High");
    CHECK_FALSE(resolved["priority"].is_string());
}

// ---------------------------------------------------------------------------
// #854 — "Use Mine" direction (keep the user's queued value): the resolved display label is the
// user's own value, so rebuilding it through the same production builder reproduces the SAME
// structured object the queue already held — it survives as an object, never flattened. (The UI
// passes a display string with kind:"scalar" for all three scalar buttons, so the service treats
// every direction identically by re-running the builder; "Use Mine" re-deriving the same shape is
// the correctness guarantee.)
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: #854 scalar resolve on structured field keeps object payload (mine)" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    StructuredFieldDeps deps;
    deps.Fields = {MakeField("priority", TrackerFieldFamily::SelectSingle)};

    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"priority", {{"id", "3"}}}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-855", "priority", payload, err, std::string(), "Low", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    // "Use Mine" passes the user's own display label ("Low").
    svc.ResolveFieldEditConflict(id, "Low", std::string(), "scalar");

    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    const auto row = svc.GetPendingFieldEdits().front();
    const nlohmann::json resolved = nlohmann::json::parse(row.FieldsPayloadJson);
    REQUIRE(resolved.contains("priority"));
    CHECK(resolved["priority"].is_object()); // object shape preserved, NOT "Low"
    CHECK(resolved["priority"].value("id", std::string()) == "Low");
}

// ---------------------------------------------------------------------------
// #854 — no-regression guard: a genuinely STRING-valued field (summary / free text) still
// resolves to a BARE STRING. The production builder for a string field returns the bare string
// under the field key, so the rebuilt payload is `{"summary":"<value>"}` — matching the old
// behaviour for the only case the pre-#854 test covered. Uses the default FakeOfflineQueueDeps
// whose BuildFieldPayload returns `{"values":[...]}`; what matters is that resolve does NOT
// special-case string fields away from the builder seam. We assert the value is not an object.
// ---------------------------------------------------------------------------
TEST_CASE("OfflineQueueServiceRuntime: #854 scalar resolve on string field stays a bare string (no regression)") {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps;
    // No catalog field for "summary" → degraded fallback writes the verbatim string, which is
    // the correct shape for a genuinely string-valued field.
    OfflineQueueService svc(deps);
    std::string err;
    const std::string payload = nlohmann::json{{"summary", "Old summary"}}.dump();
    const auto id = svc.QueueFieldEditOffline("PROJ-856", "summary", payload, err, std::string(), "Old summary", true);
    REQUIRE(err.empty());
    REQUIRE(id > 0);

    svc.ResolveFieldEditConflict(id, "New summary", std::string(), "scalar");

    REQUIRE(svc.GetPendingFieldEdits().size() == 1u);
    const auto row = svc.GetPendingFieldEdits().front();
    const nlohmann::json resolved = nlohmann::json::parse(row.FieldsPayloadJson);
    REQUIRE(resolved.contains("summary"));
    CHECK(resolved["summary"].is_string());
    CHECK(resolved["summary"].get<std::string>() == "New summary");
}
