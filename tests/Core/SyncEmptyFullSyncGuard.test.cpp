// Regression coverage for finding DR4 — a streaming full-sync that returns zero tickets must
// NOT wipe the entire SQLite ticket cache + ActiveTickets. A FullSyncCompleted=true fetch with
// an empty body (a 200-with-empty-body glitch or a transiently-broken query) leaves the worker
// keep-set empty, which marks every cached row stale. Before the fix that mass-deleted the whole
// offline cache; the empty-full-sync guard (ShouldSkipMassDeletionOnEmptyFullSync, mirroring the
// ApplyIssueFetchPack path) now treats a zero-keep full sync as suspect and holds off the wipe
// until the empty result repeats kEmptyFullSyncWipeThreshold times AND has persisted for
// kEmptyFullSyncMinStreakElapsed.
//
// The elapsed-time floor is the #2143 hardening: the count alone was satisfiable by two flaps
// seconds apart (several sync kicks can land back-to-back — pane focus changes, view re-syncs, a
// connectivity-recovery refresh), which is how a live session deleted 23 offline rows against a
// result the very next fetch contradicted.
//
// Two layers of coverage:
//   * Pure decision predicate — every branch of ShouldSkipMassDeletionOnEmptyFullSync, count and
//     elapsed-floor alike.
//   * End-to-end streaming sync — an empty full sync against FakeTrackerClient preserves the
//     cache, and back-to-back empty syncs still preserve it because the floor cannot have passed.

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

namespace {

// Elapsed values for the pure predicate's time floor. `kHeld` is comfortably past a 60s floor,
// `kBurst` is the two-flaps-in-two-seconds shape #2143 reported.
constexpr std::chrono::milliseconds kFloor = std::chrono::seconds(60);
constexpr std::chrono::milliseconds kHeld = std::chrono::seconds(120);
constexpr std::chrono::milliseconds kBurst = std::chrono::seconds(2);

} // namespace

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: a non-empty keep set never skips the deletion") {
    // keepCount > 0 means the full sync genuinely proved which rows survive, so stale rows are
    // safe to prune regardless of the empty-streak counter or how long it has run.
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(3, 10, 0, 2, kHeld, kFloor));
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(1, 10, 5, 2, kBurst, kFloor));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: nothing cached means nothing to protect") {
    // A zero-keep full sync with an already-empty cache has no rows to lose, so there is no wipe
    // to guard against.
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 0, 0, 2, kHeld, kFloor));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: a suspect empty full sync skips the wipe below threshold") {
    // Zero keeps against a populated cache while the empty streak is under the threshold — the
    // canonical DR4 scenario. The cache must be preserved however long the streak has run.
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 0, 2, kHeld, kFloor));
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 1, 2, kHeld, kFloor));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: a persistently empty project converges at the threshold") {
    // Once the empty result has repeated threshold times AND held past the floor we trust it and
    // allow the prune, so a genuinely-emptied project eventually clears its stale rows.
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 2, 2, kHeld, kFloor));
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 3, 2, kHeld, kFloor));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: #2143 — a burst that reaches the count is still refused") {
    // The regression itself: the count threshold is met, but the empty condition has only held for
    // seconds. Two flaps in quick succession are coincidence, not evidence, so the wipe must be
    // withheld until the streak actually persists.
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 23, 2, 2, kBurst, kFloor));
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 23, 9, 2, kBurst, kFloor));
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 23, 2, 2, std::chrono::milliseconds::zero(),
                                                                  kFloor));
}

TEST_CASE("ShouldSkipMassDeletionOnEmptyFullSync: the elapsed floor is inclusive at the boundary") {
    // Exactly at the floor counts as held — the predicate refuses only while STRICTLY under it, so
    // a streak that has run precisely the required duration converges rather than stalling a tick.
    CHECK_FALSE(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 2, 2, kFloor, kFloor));
    CHECK(TicketSyncService::ShouldSkipMassDeletionOnEmptyFullSync(0, 4, 2, 2, kFloor - std::chrono::milliseconds(1),
                                                                  kFloor));
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

TEST_CASE("TicketSyncService back-to-back empty full-syncs preserve the cache (#2143)" *
          doctest::test_suite("[high-risk]")) {
    // The live regression, end to end: two consecutive empty full syncs reach the count threshold
    // within milliseconds of each other. Before the elapsed-time floor that pruned the whole
    // cache (23 rows in the reported session); now the streak has not persisted long enough to be
    // believed, so every row survives.
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("KEEP-1"));
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("KEEP-2"));

    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    fake->SetFetchIssuesResult(std::vector<CachedTicket>{}, /*fullSyncCompleted=*/true);

    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;

    // First empty full sync — below the count threshold, cache preserved.
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(deps.CacheImpl->GetAllTicketIds("Jira").size() == 2);

    // Second consecutive empty full sync — the COUNT threshold is now met, which alone used to
    // authorize the wipe. The elapsed floor has not passed, so the rows must still be here.
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    std::vector<std::string> ids = deps.CacheImpl->GetAllTicketIds("Jira");
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "KEEP-1");
    CHECK(ids[1] == "KEEP-2");

    // A third back-to-back empty sync must not tip it either — the floor is wall-clock, not a
    // count in disguise, so piling on more samples cannot buy an early wipe.
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(deps.CacheImpl->GetAllTicketIds("Jira").size() == 2);

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("TicketSyncService: a non-empty sync clears the empty streak and its stamp" *
          doctest::test_suite("[high-risk]")) {
    // A recovered fetch must reset BOTH the counter and the streak stamp. If only the counter were
    // cleared, a later empty streak would inherit the old stamp and satisfy the elapsed floor
    // immediately — reintroducing #2143 by the back door.
    FakeTicketSyncDeps deps;
    deps.CacheImpl->SaveTicket("Jira", MakeTicket("KEEP-1"));

    auto* fake = static_cast<FakeTrackerClient*>(deps.BackendImpl.get());
    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "fake";
    ViewsStore views;

    // Empty, empty — streak open, count threshold reached, floor not passed.
    fake->SetFetchIssuesResult(std::vector<CachedTicket>{}, /*fullSyncCompleted=*/true);
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(deps.CacheImpl->GetAllTicketIds("Jira").size() == 1);

    // Backend recovers — this contradicts the streak outright.
    fake->SetFetchIssuesResult(std::vector<CachedTicket>{MakeTicket("KEEP-1")}, /*fullSyncCompleted=*/true);
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(deps.CacheImpl->GetAllTicketIds("Jira").size() == 1);

    // Empties resume. The streak restarted from scratch, so the cache is still protected.
    fake->SetFetchIssuesResult(std::vector<CachedTicket>{}, /*fullSyncCompleted=*/true);
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));
    CHECK(deps.CacheImpl->GetAllTicketIds("Jira").size() == 1);

    svc.CancelAndJoinActiveStreamingSync();
}
