// OfflineQueueServiceRealCacheSmoke — the ONE retained service-vs-real-SQLite case (ADR-0020 /
// ilocalcache-seam grill Q3, option 3).
//
// Every other service TU now runs on the in-memory FakeSyncCache; the per-method contract
// suite (SyncCacheContract.test.cpp) pins fake fidelity method-by-method. This smoke is the
// PER-SERVICE-PATH backstop: it drives a full OfflineQueueService create replay end-to-end
// against a real `:memory:` LocalCacheManager, catching any fake-vs-real divergence the
// per-method assertions missed (interaction effects, transaction boundaries).
//
// SQLite-by-design: named-exempt from the construction/direct-include purity gate.

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "ISyncCache.h"
#include "IssueDraft.h"
#include "LocalCacheManager.h"
#include "OfflineQueueService.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

using smatchet_tests::FakeOfflineQueueDeps;
using smatchet_tests::OfflineQueueTestEnvGuard;

namespace {

/// FakeOfflineQueueDeps with the cache swapped back to a REAL :memory: LocalCacheManager —
/// everything else (backend, catalog, background-task runner) stays the in-memory fake.
class RealCacheOfflineQueueDeps : public FakeOfflineQueueDeps {
  public:
    // NOTE: the base's `CacheImpl` (FakeSyncCache) still exists but is ORPHANED here — never
    // read `deps.CacheImpl` in this TU (it would silently target the fake, not the real cache).
    ISyncCache* Cache() override { return &real_; }
    // DR6: match the overridden Cache() — a non-owning aliasing shared_ptr over the SAME real_
    // cache (the fixture owns real_ for its lifetime). Without this, the base CacheShared() would
    // hand back the orphaned fake cache, not real_.
    std::shared_ptr<ISyncCache> CacheShared() override {
        return std::shared_ptr<ISyncCache>(std::shared_ptr<void>(), &real_);
    }
    LocalCacheManager& RealCache() { return real_; }

  private:
    LocalCacheManager real_{":memory:"};
};

} // namespace

TEST_CASE("real-cache smoke: offline create queues, replays, seeds the ticket, and consumes the row" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    RealCacheOfflineQueueDeps deps; // context key "Jira"
    deps.BackendImpl->SetBuildCreatePayloadResult(true, nlohmann::json{{"fields", {{"summary", "X"}}}});
    TrackerField f;
    f.Id = "summary";
    f.Name = "summary";
    f.Type = "string";
    deps.Fields = {f};
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-77");

    OfflineQueueService svc(deps);
    IssueDraft draft;
    draft.ProjectKey = "PROJ";
    draft.IssueTypeId = "10001";
    draft.IssueTypeName = "Story";
    draft.FieldValues["summary"] = "real cache smoke";
    REQUIRE(svc.QueueCreateOffline(draft) > 0);

    // Row landed in real SQLite under the context's key.
    REQUIRE(deps.RealCache().LoadPendingCreates().size() == 1u);
    CHECK(deps.RealCache().LoadPendingCreates().front().BackendKey == "Jira");

    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();

    // Replay completed against the fake backend; the SQLite row was consumed and the created
    // ticket was seeded into the real cache under the captured key.
    CHECK(deps.BackendImpl->CreateIssueCallCount() == 1u);
    CHECK(deps.RealCache().LoadPendingCreates().empty());
    CachedTicket cached;
    REQUIRE(deps.RealCache().TryGetTicket("Jira", "PROJ-77", cached));
    CHECK(deps.RealCache().GetAllTickets("GitHub").empty());
    CHECK(svc.GetDeadPendingCreateCount() == 0u);
}
