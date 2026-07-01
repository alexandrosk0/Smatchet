// Pending-queue backend_key + backend-scoped replay — multi-grid Slice 1c (ADR-0018 decision
// 4, docs/plans/multi-grid-tabs-slice1-design.md § 3.5 "Pending queues" +
// "Replay-matching rule").
//
// Covers the SERVICE-level bucket-A cases the design mandates (pure-logic on FakeSyncCache —
// ADR-0020 / ilocalcache-seam):
//   * Replay matching: with rows queued under two backend keys, context A's replay tick
//     touches only A-rows; B-rows stay queued untouched (never replayed against a wrong
//     backend, never dropped). An empty-key (corrupt) row is treated as no-match.
//   * Dead-letter restore VIA THE SERVICE preserves the original key when the focused differs.
//
// The cache-IMPL siblings are covered separately: the ADD-COLUMN/stamp migration cases live in
// LocalCacheTicketsV2Migration.test.cpp (file-backed DB + off-interface RunOneTime* methods);
// the cache-level dead-letter key/bases round-trip is pinned dual-impl in
// SyncCacheContract.test.cpp.

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/OfflineQueueTestEnv.h"

#include "IssueDraft.h"
#include "OfflineQueueService.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>

using smatchet_tests::FakeOfflineQueueDeps;
using smatchet_tests::TestEnvGuard;

namespace {

IssueDraft MakeDraft(const std::string& summary) {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.IssueTypeName = "Story";
    d.FieldValues["summary"] = summary;
    return d;
}

void PrimeCreateHappy(FakeOfflineQueueDeps& deps) {
    deps.BackendImpl->SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", {{"summary", "X"}}}});
    TrackerField f;
    f.Id = "summary";
    f.Name = "summary";
    f.Type = "string";
    deps.Fields = {f};
}

} // namespace

TEST_CASE("queue backend_key replay matching: context A's tick replays only A-rows; B-rows stay queued" *
          doctest::test_suite("[high-risk]")) {
    TestEnvGuard guard;
    FakeOfflineQueueDeps deps; // CacheBackendKeyImpl defaults to "Jira" — this context is "A"
    PrimeCreateHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-1");

    OfflineQueueService svc(deps);
    // A-row through the service (stamped with the context's key)...
    REQUIRE(svc.QueueCreateOffline(MakeDraft("jira row")) > 0);
    CHECK(deps.CacheImpl->LoadPendingCreates().front().BackendKey == "Jira");
    // ...plus a B-row and a corrupt empty-key row, queued directly at the cache layer.
    const std::int64_t planeId =
        deps.CacheImpl->EnqueuePendingCreate("Plane", IssueDraftHelpers::ToJson(MakeDraft("plane row")));
    const std::int64_t emptyId =
        deps.CacheImpl->EnqueuePendingCreate(std::string(), IssueDraftHelpers::ToJson(MakeDraft("corrupt row")));
    REQUIRE(svc.GetPendingCreateCount() == 3u);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    // Exactly the Jira row replayed; the Plane + empty-key rows stay queued UNTOUCHED
    // (attempts not bumped, never dead-lettered, never replayed against the wrong backend).
    CHECK(deps.BackendImpl->CreateIssueCallCount() == 1u);
    const auto remaining = deps.CacheImpl->LoadPendingCreates();
    REQUIRE(remaining.size() == 2);
    CHECK(remaining[0].Id == planeId);
    CHECK(remaining[0].BackendKey == "Plane");
    CHECK(remaining[0].Attempts == 0);
    CHECK(remaining[1].Id == emptyId);
    CHECK(remaining[1].Attempts == 0);
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
}

TEST_CASE("queue backend_key replay matching: field-edit tick is backend-scoped too" *
          doctest::test_suite("[high-risk]")) {
    TestEnvGuard guard;
    FakeOfflineQueueDeps deps; // context key "Jira"
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    OfflineQueueService svc(deps);
    std::string err;
    REQUIRE(svc.QueueFieldEditOffline("PROJ-1", "summary", nlohmann::json{{"summary", "v"}}.dump(), err,
                                      std::string()) > 0);
    REQUIRE(err.empty());
    const std::int64_t planeId =
        deps.CacheImpl->EnqueuePendingFieldEdit("Plane", "OTHER-9", "summary", nlohmann::json{{"summary", "w"}}.dump());
    REQUIRE(svc.GetPendingFieldEdits().size() == 2u);

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();

    // Only the Jira edit was PUT; the Plane edit stays queued untouched.
    CHECK(deps.BackendImpl->UpdateIssueFieldsCallCount() == 1u);
    const auto remaining = deps.CacheImpl->LoadPendingFieldEdits();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining.front().Id == planeId);
    CHECK(remaining.front().BackendKey == "Plane");
    CHECK(remaining.front().Attempts == 0);
    CHECK(svc.GetDeadPendingFieldEdits().empty());
}

TEST_CASE("dead-letter restore via the SERVICE preserves the original backend key when the focused key differs "
          "(CR-951-1) and re-queues restored creates as fresh creates" *
          doctest::test_suite("[high-risk]")) {
    TestEnvGuard guard;
    FakeOfflineQueueDeps deps; // focused-context key is "Jira" — every dead row below is NOT Jira
    OfflineQueueService svc(deps);

    // Dead CREATE queued under "Plane", payload carrying a stale update identity.
    IssueDraft draft = MakeDraft("plane create");
    draft.ExistingIssueKey = "PLANE-9";
    draft.FieldValues["issuekey"] = "PLANE-9";
    draft.FieldValues["key"] = "PLANE-9";
    const std::int64_t createId = deps.CacheImpl->EnqueuePendingCreate("Plane", IssueDraftHelpers::ToJson(draft));
    deps.CacheImpl->ArchivePendingCreate(createId, "max_attempts", "terminal");
    REQUIRE(deps.CacheImpl->LoadPendingCreates().empty());

    const auto createSummary = svc.RestoreDeadPendingCreates({createId});
    CHECK(createSummary.Restored == 1);
    CHECK(createSummary.Failed == 0);
    CHECK(deps.CacheImpl->LoadDeadPendingCreates().empty());
    const auto restoredCreates = deps.CacheImpl->LoadPendingCreates();
    REQUIRE(restoredCreates.size() == 1);
    CHECK(restoredCreates.front().BackendKey == "Plane"); // NOT the deps' focused "Jira"
    CHECK(restoredCreates.front().Attempts == 0);
    // Fresh-create scrub preserved from the UI restore contract.
    IssueDraft restoredDraft;
    std::string parseErr;
    REQUIRE(IssueDraftHelpers::FromJson(restoredCreates.front().Payload, restoredDraft, parseErr));
    CHECK(restoredDraft.ExistingIssueKey.empty());
    CHECK(restoredDraft.FieldValues.count("issuekey") == 0);
    CHECK(restoredDraft.FieldValues.count("key") == 0);
    CHECK(restoredDraft.FieldValues.at("summary") == "plane create");

    // Dead FIELD EDIT queued under "GitHub" — the new key-preserving twin.
    const std::int64_t editId = deps.CacheImpl->EnqueuePendingFieldEdit(
        "GitHub", "OWN/REPO#7", "summary", "{\"summary\":\"v\"}", "rich base", "scalar base", true);
    deps.CacheImpl->ArchivePendingFieldEdit(editId, "replay_rejected", "terminal");
    REQUIRE(deps.CacheImpl->LoadPendingFieldEdits().empty());

    const auto editSummary = svc.RestoreDeadPendingFieldEdits({editId});
    CHECK(editSummary.Restored == 1);
    CHECK(editSummary.Failed == 0);
    CHECK(deps.CacheImpl->LoadDeadPendingFieldEdits().empty());
    const auto restoredEdits = deps.CacheImpl->LoadPendingFieldEdits();
    REQUIRE(restoredEdits.size() == 1);
    CHECK(restoredEdits.front().BackendKey == "GitHub"); // NOT the deps' focused "Jira"
    CHECK(restoredEdits.front().Attempts == 0);
    CHECK(restoredEdits.front().OriginalRichValue == "rich base");
    CHECK(restoredEdits.front().OriginalValue == "scalar base");
    CHECK(restoredEdits.front().HasOriginalValue);

    // Missing original id: counted Failed, nothing mutated.
    const auto missSummary = svc.RestoreDeadPendingFieldEdits({987654});
    CHECK(missSummary.Restored == 0);
    CHECK(missSummary.Failed == 1);
}
