// Two live backends queuing + replaying at once — multi-grid Slice 4 (plan item 20, final
// clause: "offline-queue concurrent replay (BackendKey-matched, landed S1) exercised with two
// live backends queuing at once").
//
// Topology under test (the realistic multi-grid shape): ONE shared queue DB
// (production: `LocalCacheManager`; here the contract-suite-verified `FakeSyncCache` — ADR-0020)
// holds rows for every backend, namespaced by `backend_key` (Slice 1c);
// each `GridLiveContext` owns its OWN `OfflineQueueService` over a per-context deps adapter
// that carries that context's backend key + backend handle (Slice 3). These tests put a "Jira"
// pane and a "GitHub" pane over the same cache and prove the strict-equality replay filter
// (`FilterRowsToReplayBackendKey`, Slice 1c) holds when both panes queue interleaved and then
// replay — the core multi-grid isolation invariant: each backend replays ONLY its own rows,
// never the other's, under any interleaving.
//
// Concurrency model: the two services share a SINGLE sync cache (production: one SQLite
// connection). A genuine two-thread replay would drive that one connection from two threads
// with no connection-level serialisation — a configuration that is itself racy, so a
// real-thread test could not legitimately assert "clean". We therefore model logical
// concurrency as a DETERMINISTIC INTERLEAVE (both panes queue while offline; each pane's
// replay is scheduled independently and the ticks are interleaved). This exercises exactly the
// property a concurrent scheduler must preserve — backend_key isolation — without asserting on
// a known-racy setup. The real-thread / ThreadSanitizer pass over concurrent replay is gated
// to Slice 5 (the plan already lands TSan there).

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "IssueDraft.h"
#include "OfflineQueueService.h"

#include <nlohmann/json.hpp>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using smatchet_tests::FakeOfflineQueueDeps;
using smatchet_tests::FakeTrackerClient;
using smatchet_tests::OfflineQueueTestEnvGuard;

namespace {

// Two services share ONE queue DB (the multi-grid topology: one cache, N per-context services
// keyed by backend). `SharedCacheDeps` points its `Cache()` at the externally-owned shared
// manager and carries this context's backend key + its own `FakeTrackerClient` backend.
class SharedCacheDeps : public FakeOfflineQueueDeps {
  public:
    SharedCacheDeps(ISyncCache* shared, std::string key) : shared_(shared) { CacheBackendKeyImpl = std::move(key); }
    ISyncCache* Cache() override { return shared_; }

  private:
    ISyncCache* shared_;
};

IssueDraft MakeDraft(const std::string& summary) {
    IssueDraft d;
    d.ProjectKey = "PROJ";
    d.IssueTypeId = "10001";
    d.IssueTypeName = "Story";
    d.FieldValues["summary"] = summary;
    return d;
}

// Prime a deps' backend so `QueueCreateOffline` replay can build + create. The build payload is
// tagged per backend so a leaked cross-backend create would be visible in the call recording.
void PrimeCreate(FakeOfflineQueueDeps& deps, const std::string& tag) {
    deps.BackendImpl->SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", {{"origin", tag}}}});
    TrackerField f;
    f.Id = "summary";
    f.Name = "summary";
    f.Type = "string";
    deps.Fields = {f};
}

std::string FieldPayload(const std::string& v) { return nlohmann::json{{"summary", v}}.dump(); }

} // namespace

TEST_CASE("two-backend interleaved offline replay: each backend replays ONLY its own backend_key rows; "
          "the other backend's rows stay queued untouched (multi-grid Slice 4 isolation)" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    smatchet_tests::FakeSyncCache sharedCache; // ONE shared queue store for both panes

    SharedCacheDeps jira(&sharedCache, "Jira");
    SharedCacheDeps github(&sharedCache, "GitHub");
    PrimeCreate(jira, "jira-origin");
    PrimeCreate(github, "github-origin");

    // Script two create successes + two field-edit successes per backend.
    jira.BackendImpl->EnqueueCreateIssueSuccess("JIRA-101");
    jira.BackendImpl->EnqueueCreateIssueSuccess("JIRA-102");
    jira.BackendImpl->EnqueueUpdateIssueFieldsSuccess();
    jira.BackendImpl->EnqueueUpdateIssueFieldsSuccess();
    github.BackendImpl->EnqueueCreateIssueSuccess("GH-1");
    github.BackendImpl->EnqueueCreateIssueSuccess("GH-2");
    github.BackendImpl->EnqueueUpdateIssueFieldsSuccess();
    github.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    OfflineQueueService svcJira(jira);
    OfflineQueueService svcGitHub(github);

    // --- Both panes queue while offline, fully INTERLEAVED (the logical-concurrency schedule).
    std::string err;
    const std::int64_t jc1 = svcJira.QueueCreateOffline(MakeDraft("j-create-1"));
    const std::int64_t gc1 = svcGitHub.QueueCreateOffline(MakeDraft("g-create-1"));
    const std::int64_t je1 =
        svcJira.QueueFieldEditOffline("JIRA-5", "summary", FieldPayload("jv1"), err, std::string());
    const std::int64_t ge1 =
        svcGitHub.QueueFieldEditOffline("OWN/REPO#5", "summary", FieldPayload("gv1"), err, std::string());
    const std::int64_t gc2 = svcGitHub.QueueCreateOffline(MakeDraft("g-create-2"));
    const std::int64_t jc2 = svcJira.QueueCreateOffline(MakeDraft("j-create-2"));
    const std::int64_t ge2 =
        svcGitHub.QueueFieldEditOffline("OWN/REPO#6", "summary", FieldPayload("gv2"), err, std::string());
    const std::int64_t je2 =
        svcJira.QueueFieldEditOffline("JIRA-6", "summary", FieldPayload("jv2"), err, std::string());
    REQUIRE(jc1 > 0);
    REQUIRE(gc1 > 0);
    REQUIRE(je1 > 0);
    REQUIRE(ge1 > 0);
    REQUIRE(gc2 > 0);
    REQUIRE(jc2 > 0);
    REQUIRE(ge2 > 0);
    REQUIRE(je2 > 0);

    // The shared queue now holds 4 creates + 4 edits, each stamped with its OWNING pane's key.
    {
        const auto creates = sharedCache.LoadPendingCreates();
        REQUIRE(creates.size() == 4);
        const auto edits = sharedCache.LoadPendingFieldEdits();
        REQUIRE(edits.size() == 4);
    }

    // --- Jira comes online and replays. Strict-equality filter ⇒ only the 2 Jira rows fire.
    svcJira.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svcJira.TickOfflineCreates();
    svcJira.TickOfflineFieldEdits();

    // Only the Jira backend was touched; the GitHub backend never saw a single call.
    CHECK(jira.BackendImpl->CreateIssueCallCount() == 2u);
    CHECK(jira.BackendImpl->UpdateIssueFieldsCallCount() == 2u);
    CHECK(github.BackendImpl->CreateIssueCallCount() == 0u);
    CHECK(github.BackendImpl->UpdateIssueFieldsCallCount() == 0u);
    // Every create the Jira backend processed carried the Jira-origin build payload — no
    // GitHub-origin payload ever reached the wrong backend.
    for (const auto& call : jira.BackendImpl->CreateIssueCalls()) {
        CHECK(call.Fields.at("fields").at("origin") == "jira-origin");
    }

    // The GitHub rows remain queued, UNTOUCHED: exactly the GitHub ids, GitHub key, attempts 0,
    // nothing dead-lettered.
    {
        const auto remCreates = sharedCache.LoadPendingCreates();
        REQUIRE(remCreates.size() == 2);
        CHECK(remCreates[0].Id == gc1);
        CHECK(remCreates[1].Id == gc2);
        for (const auto& r : remCreates) {
            CHECK(r.BackendKey == "GitHub");
            CHECK(r.Attempts == 0);
        }
        const auto remEdits = sharedCache.LoadPendingFieldEdits();
        REQUIRE(remEdits.size() == 2);
        CHECK(remEdits[0].Id == ge1);
        CHECK(remEdits[1].Id == ge2);
        for (const auto& r : remEdits) {
            CHECK(r.BackendKey == "GitHub");
            CHECK(r.Attempts == 0);
        }
        CHECK(svcGitHub.GetDeadPendingCreateCount() == 0u);
        CHECK(svcGitHub.GetDeadPendingFieldEdits().empty());
    }

    // --- GitHub comes online and replays. Its 2 rows fire; the Jira backend is unchanged.
    svcGitHub.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svcGitHub.TickOfflineCreates();
    svcGitHub.TickOfflineFieldEdits();

    CHECK(github.BackendImpl->CreateIssueCallCount() == 2u);
    CHECK(github.BackendImpl->UpdateIssueFieldsCallCount() == 2u);
    CHECK(jira.BackendImpl->CreateIssueCallCount() == 2u); // unchanged — Jira backend never re-touched
    CHECK(jira.BackendImpl->UpdateIssueFieldsCallCount() == 2u);
    for (const auto& call : github.BackendImpl->CreateIssueCalls()) {
        CHECK(call.Fields.at("fields").at("origin") == "github-origin");
    }

    // The shared queue is now fully drained, nothing dead-lettered on either side.
    CHECK(sharedCache.LoadPendingCreates().empty());
    CHECK(sharedCache.LoadPendingFieldEdits().empty());
    CHECK(sharedCache.LoadDeadPendingCreates().empty());
    CHECK(sharedCache.LoadDeadPendingFieldEdits().empty());
}

TEST_CASE("two-backend dead-letter coexistence: dead rows under both keys restore under their OWN backend_key, "
          "restoring one never disturbs the other (multi-grid Slice 4)" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    smatchet_tests::FakeSyncCache sharedCache;

    SharedCacheDeps jira(&sharedCache, "Jira");
    SharedCacheDeps github(&sharedCache, "GitHub");
    OfflineQueueService svcJira(jira);
    OfflineQueueService svcGitHub(github);

    // Dead CREATE under each key, present simultaneously.
    const std::int64_t jCreate =
        sharedCache.EnqueuePendingCreate("Jira", IssueDraftHelpers::ToJson(MakeDraft("jdead")));
    const std::int64_t gCreate =
        sharedCache.EnqueuePendingCreate("GitHub", IssueDraftHelpers::ToJson(MakeDraft("gdead")));
    sharedCache.ArchivePendingCreate(jCreate, "max_attempts", "terminal");
    sharedCache.ArchivePendingCreate(gCreate, "max_attempts", "terminal");

    // Dead FIELD EDIT under each key, present simultaneously (with merge bases).
    const std::int64_t jEdit =
        sharedCache.EnqueuePendingFieldEdit("Jira", "JIRA-9", "summary", FieldPayload("jx"), "jrich", "jscalar", true);
    const std::int64_t gEdit = sharedCache.EnqueuePendingFieldEdit("GitHub", "OWN/REPO#9", "summary",
                                                                   FieldPayload("gx"), "grich", "gscalar", true);
    sharedCache.ArchivePendingFieldEdit(jEdit, "replay_rejected", "terminal");
    sharedCache.ArchivePendingFieldEdit(gEdit, "replay_rejected", "terminal");

    REQUIRE(sharedCache.LoadDeadPendingCreates().size() == 2);
    REQUIRE(sharedCache.LoadDeadPendingFieldEdits().size() == 2);

    // Restore ONLY the GitHub dead create, via the GitHub service. It re-queues under "GitHub";
    // the Jira dead create is wholly undisturbed (still dead, still "Jira").
    const auto gSummary = svcGitHub.RestoreDeadPendingCreates({gCreate});
    CHECK(gSummary.Restored == 1);
    CHECK(gSummary.Failed == 0);
    {
        const auto active = sharedCache.LoadPendingCreates();
        REQUIRE(active.size() == 1);
        CHECK(active.front().BackendKey == "GitHub"); // restored under its OWN key (only gCreate was dead+GitHub)
        CHECK(active.front().Attempts == 0);
        const auto dead = sharedCache.LoadDeadPendingCreates();
        REQUIRE(dead.size() == 1);
        CHECK(dead.front().OriginalId == jCreate);
        CHECK(dead.front().BackendKey == "Jira"); // Jira dead row untouched + key intact
    }

    // Restore ONLY the Jira dead edit, via the Jira service. Mirror invariant for field edits:
    // re-queues under "Jira" with both bases; the GitHub dead edit stays dead under "GitHub".
    const auto jSummary = svcJira.RestoreDeadPendingFieldEdits({jEdit});
    CHECK(jSummary.Restored == 1);
    CHECK(jSummary.Failed == 0);
    {
        const auto active = sharedCache.LoadPendingFieldEdits();
        REQUIRE(active.size() == 1);
        CHECK(active.front().BackendKey == "Jira"); // restored under its OWN key
        CHECK(active.front().IssueKey == "JIRA-9"); // identity: the Jira edit, not the GitHub one
        CHECK(active.front().OriginalRichValue == "jrich");
        CHECK(active.front().OriginalValue == "jscalar");
        CHECK(active.front().HasOriginalValue);
        const auto dead = sharedCache.LoadDeadPendingFieldEdits();
        REQUIRE(dead.size() == 1);
        CHECK(dead.front().OriginalId == gEdit);
        CHECK(dead.front().BackendKey == "GitHub"); // GitHub dead edit untouched + key intact
    }
}

TEST_CASE("two-backend swap-mid-replay: a deferred Jira replay completes ONLY against Jira rows even after the "
          "Jira backend is swapped; GitHub rows + the GitHub backend are never touched (multi-grid Slice 4)" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    smatchet_tests::FakeSyncCache sharedCache;

    SharedCacheDeps jira(&sharedCache, "Jira");
    SharedCacheDeps github(&sharedCache, "GitHub");
    PrimeCreate(jira, "jira-origin");
    jira.BackendImpl->EnqueueCreateIssueSuccess("JIRA-201");

    // Defer the Jira replay worker so a backend swap can land inside the replay window (the
    // Slice-3 latch seam). The GitHub service stays inline (default runner).
    std::function<void()> capturedJira;
    jira.BackgroundTaskRunner = [&capturedJira](std::function<void()> t) { capturedJira = std::move(t); };

    OfflineQueueService svcJira(jira);
    OfflineQueueService svcGitHub(github);

    const std::int64_t jc = svcJira.QueueCreateOffline(MakeDraft("jira swap victim"));
    const std::int64_t gc = svcGitHub.QueueCreateOffline(MakeDraft("github bystander"));
    REQUIRE(jc > 0);
    REQUIRE(gc > 0);

    svcJira.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svcJira.TickOfflineCreates();
    REQUIRE(capturedJira); // worker latched the Jira backend + deferred — swap window open

    // Swap the Jira backend mid-replay; the latched strong handle inside `capturedJira` keeps
    // the old Jira backend alive. The GitHub pane is entirely independent.
    std::weak_ptr<FakeTrackerClient> oldJira = jira.BackendImpl;
    jira.BackendImpl = std::make_shared<FakeTrackerClient>();
    REQUIRE_FALSE(oldJira.expired());

    capturedJira(); // Jira replay runs against the latched (old) Jira backend

    {
        std::shared_ptr<FakeTrackerClient> old = oldJira.lock();
        REQUIRE(old);
        CHECK(old->CreateIssueCallCount() == 1u);              // Jira row replayed
        CHECK(jira.BackendImpl->CreateIssueCallCount() == 0u); // new Jira backend untouched
    }
    CHECK(github.BackendImpl->CreateIssueCallCount() == 0u); // GitHub backend never touched

    // The GitHub row is still queued under "GitHub", attempts 0 — never swept into the Jira
    // replay despite running across the swap.
    const auto remaining = sharedCache.LoadPendingCreates();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining.front().Id == gc);
    CHECK(remaining.front().BackendKey == "GitHub");
    CHECK(remaining.front().Attempts == 0);

    // Dropping the worker capture releases the latched old-Jira handle — no leak.
    capturedJira = nullptr;
    CHECK(oldJira.expired());
}
