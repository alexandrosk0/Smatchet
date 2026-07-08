// Regression coverage for finding DR4 — a streaming full-sync that returns zero tickets must
// NOT wipe the entire SQLite ticket cache + ActiveTickets. A FullSyncCompleted=true fetch with
// an empty body (a 200-with-empty-body glitch or a transiently-broken query) leaves the worker
// keep-set empty, which marks every cached row stale. Before the fix that mass-deleted the whole
// offline cache; the empty-full-sync guard (ShouldSkipMassDeletionOnEmptyFullSync, mirroring the
// ApplyIssueFetchPack path) now treats a zero-keep full sync as suspect and holds off the wipe
// until the empty result repeats kEmptyFullSyncWipeThreshold times.
//
// Two layers of coverage:
//   * Pure decision predicate — every branch of ShouldSkipMassDeletionOnEmptyFullSync.
//   * End-to-end streaming sync — an empty full sync against FakeTrackerClient preserves the
//     cache once, then converges (wipes) after the threshold is reached.

#include "../support/FakeTicketSyncDeps.h"
#include "../support/FakeTrackerClient.h"

#include "CachedTicketTypes.h"
#include "Config/ConfigManager.h"
#include "TicketSyncService.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using smatchet_tests::FakeTicketSyncDeps;
using smatchet_tests::FakeTrackerClient;

namespace {

CachedTicket MakeTicket(const std::string& id, const std::string& summary = "summary") {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = summary;
    t.fieldValues["status"] = "Open";
    return t;
}

template <typename Pred> bool SpinUntil(TicketSyncService& svc, Pred pred, int maxTicks = 400) {
    for (int i = 0; i < maxTicks; ++i) {
        svc.TickStreamingApply();
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: a non-empty keep set never skips the deletion") {
    // keepCount > 0 means the full sync genuinely proved which rows survive, so stale rows are
    // safe to prune regardless of the empty-streak counter.
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(3, 10, 0, 2));
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(1, 10, 5, 2));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: nothing cached means nothing to protect") {
    // A zero-keep full sync with an already-empty cache has no rows to lose, so there is no wipe
    // to guard against.
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 0, 0, 2));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: a suspect empty full sync skips the wipe below threshold") {
    // Zero keeps against a populated cache while the empty streak is under the threshold — the
    // canonical DR4 scenario. The cache must be preserved.
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 0, 2));
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 1, 2));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: a persistently empty project converges at the threshold") {
    // Once the empty result has repeated threshold times we trust it and allow the prune so a
    // genuinely-emptied project eventually clears its stale rows.
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 2, 2));
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 3, 2));
}

TEST_CASE("TicketSyncService streaming empty full-sync preserves the cache instead of wiping it" *
          doctest::test_suite("[high-risk]")) {
    FakeTicketSyncDeps deps;
    // Cache + in-memory rows an offline user is relying on.
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("KEEP-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("KEEP-2"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("KEEP-1"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("KEEP-2"));

    // The backend returns a completed full sync with ZERO tickets (the glitch DR4 describes).
    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult(std::vector<CachedTicket>{}, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "fake"; // no backend swap; normalizes to the default cache bucket ("Jira")
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));

    // The suspect empty full sync must NOT have deleted anything.
    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "KEEP-1");
    CHECK(ids[1] == "KEEP-2");
    CHECK(deps.ActiveTicketsImpl.size() == 2);

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService streaming empty full-sync converges and prunes after the wipe threshold" *
          doctest::test_suite("[high-risk]")) {
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("STALE-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("STALE-2"));

    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult(std::vector<CachedTicket>{}, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;

    // First empty full sync — below threshold, cache preserved.
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(deps.CacheImpl->GetAllTicketIds("Jira").size() == 2);

    // Second consecutive empty full sync — the streak reaches kEmptyFullSyncWipeThreshold (2),
    // so the previously-suspect empty result is now trusted and the stale rows are pruned.
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive() && deps.CacheImpl->GetAllTicketIds("Jira").empty(); }));
    CHECK(deps.CacheImpl->GetAllTicketIds("Jira").empty());

    svc.CancelAndJoinActiveStreamingSync();
}
