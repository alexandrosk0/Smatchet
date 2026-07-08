// Backend swap DURING offline replay — multi-grid Slice 3 pre-staged debt fix
// (debt.md 2026-06-07: "OfflineQueueService replay workers capture raw
// deps_.Reader()/Mutations() pointers across async").
//
// The replay ticks now latch STRONG role handles (`IOfflineQueueDeps::ReaderShared` /
// `MutationsShared`, aliasing shared_ptr onto the backend in production) before spawning the
// background worker, so a live tracker swap — or a Slice-3 pane-context retirement — mid-replay
// can never dangle the subobject pointers. These bucket-A cases pin that contract:
//   * swap-during-create-replay: the replay completes against the LATCHED (old) backend,
//     the new backend is untouched, and the old backend stays alive exactly as long as the
//     worker capture holds it (use-after-free class — Pillar 3).
//   * swap-to-null (context retirement shape): same latch contract via the field-edit tick.
//
// Deferral of the worker is done through the fixture's `BackgroundTaskRunner` seam: capturing
// the task instead of running it inline opens the window in which the swap happens.

#include "../support/FakeOfflineQueueDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "IssueDraft.h"
#include "OfflineQueueService.h"

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

using smatchet_tests::FakeOfflineQueueDeps;
using smatchet_tests::FakeTrackerClient;
using smatchet_tests::OfflineQueueTestEnvGuard;

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

TEST_CASE("offline create replay survives a backend swap mid-replay: completes against the latched backend" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps; // context key "Jira"
    PrimeCreateHappy(deps);
    deps.BackendImpl->EnqueueCreateIssueSuccess("PROJ-9");

    // Defer the replay worker: TickOfflineCreates latches the role handles + builds the
    // task, but the task body runs only when we invoke `captured` below.
    std::function<void()> captured;
    deps.BackgroundTaskRunner = [&captured](std::function<void()> t) { captured = std::move(t); };

    OfflineQueueService svc(deps);
    REQUIRE(svc.QueueCreateOffline(MakeDraft("swap victim")) > 0);
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineCreates();
    REQUIRE(captured); // worker latched + deferred — the swap window is open

    // SWAP: the fixture drops its strong ref to the old backend. Pre-fix, the worker held a
    // raw ITrackerIssueMutations* into this freed object (use-after-free); post-fix the
    // latched shared_ptr inside `captured` is now the ONLY thing keeping it alive.
    std::weak_ptr<FakeTrackerClient> oldWeak = deps.BackendImpl;
    deps.BackendImpl = std::make_shared<FakeTrackerClient>();
    REQUIRE_FALSE(oldWeak.expired()); // latched handle holds the old backend

    captured(); // replay runs — against the LATCHED backend, no dangling

    {
        std::shared_ptr<FakeTrackerClient> old = oldWeak.lock();
        REQUIRE(old);
        CHECK(old->CreateIssueCallCount() == 1u);              // replay completed against the old backend
        CHECK(deps.BackendImpl->CreateIssueCallCount() == 0u); // the new backend was never touched
    }
    CHECK(deps.CacheImpl->LoadPendingCreates().empty()); // row consumed (successful replay)

    // Dropping the worker capture releases the latched handles — no leak, no graveyard need.
    captured = nullptr;
    CHECK(oldWeak.expired());
}

TEST_CASE("offline field-edit replay survives a swap-to-null (context-retirement shape) mid-replay" *
          doctest::test_suite("[high-risk]")) {
    OfflineQueueTestEnvGuard guard;
    FakeOfflineQueueDeps deps; // context key "Jira"
    deps.BackendImpl->EnqueueUpdateIssueFieldsSuccess();

    std::function<void()> captured;
    deps.BackgroundTaskRunner = [&captured](std::function<void()> t) { captured = std::move(t); };

    OfflineQueueService svc(deps);
    std::string err;
    REQUIRE(svc.QueueFieldEditOffline("PROJ-1", "summary", "{\"summary\":\"v\"}", err, std::string()) > 0);
    svc.RestartReplayTimersNow(std::chrono::steady_clock::now());
    svc.TickOfflineFieldEdits();
    REQUIRE(captured);

    // Retirement shape: the backend slot empties entirely (no successor). The latched
    // handles inside the deferred worker are the only owners left.
    std::weak_ptr<FakeTrackerClient> oldWeak = deps.BackendImpl;
    deps.BackendImpl.reset();
    REQUIRE_FALSE(oldWeak.expired());

    captured(); // must not crash; replays against the latched backend

    {
        std::shared_ptr<FakeTrackerClient> old = oldWeak.lock();
        REQUIRE(old);
        CHECK(old->UpdateIssueFieldsCallCount() == 1u);
    }
    captured = nullptr;
    CHECK(oldWeak.expired());
}
