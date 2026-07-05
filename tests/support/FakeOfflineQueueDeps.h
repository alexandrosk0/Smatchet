#ifndef SMATCHET_TESTS_FAKE_OFFLINE_QUEUE_DEPS_H
#define SMATCHET_TESTS_FAKE_OFFLINE_QUEUE_DEPS_H

// FakeOfflineQueueDeps — header-only in-memory implementation of `IOfflineQueueDeps` for the
// doctest rig. Backs the cache with the in-memory `FakeSyncCache` (ADR-0020 — SQLite-free,
// contract-suite-verified against the real LocalCacheManager) and the tracker backend
// with a `FakeTrackerClient`; the deferred-notify latch and refresh-data hook are simple
// counters / callbacks. `LaunchBackgroundTask` runs the task synchronously on the caller —
// tests that need async behaviour can override this by swapping `BackgroundTaskRunner`.
//
// This fixture is the test-side counterpart of `AppControllerDepsAdapter`. Any new method
// added to `IOfflineQueueDeps` MUST be implemented here too — the override list mirrors the
// production adapter.

#include "FakeTrackerClient.h"
#include "IOfflineQueueDeps.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "IssueDraft.h"
#include "FakeSyncCache.h"
#include "ISyncCache.h"
#include "TrackerFieldSchema.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace smatchet_tests {

class FakeOfflineQueueDeps : public IOfflineQueueDeps {
  public:
    /// In-memory sync cache (no SQLite). Owned by the fixture so each test starts with an empty queue.
    std::unique_ptr<FakeSyncCache> CacheImpl{std::make_unique<FakeSyncCache>()};
    /// In-memory tracker backend. Tests script `CreateIssue` / `UpdateIssueFields` responses
    /// before running the service-under-test.
    std::shared_ptr<FakeTrackerClient> BackendImpl{std::make_shared<FakeTrackerClient>()};
    /// Test-controlled field catalog. Default empty — tests that exercise `TickOfflineCreates`
    /// against required-field validation populate this before each tick.
    std::vector<TrackerField> Fields;
    /// Test-controlled required-field set. Returned verbatim by `GetRequiredFieldSet`.
    RequiredFieldSet Required;
    /// Counts of side-effects so tests can assert "RefreshLocalData ran exactly N times".
    /// RefreshLocalDataCalls counts APPLIED refreshes (unchecked calls + checked calls whose
    /// generation matched at apply time — mirroring production, where the checked overload's
    /// under-lock re-check drops the replace on mismatch).
    int RefreshLocalDataCalls = 0;
    /// Checked-overload bookkeeping (issue #1081): how many checked
    /// refreshes arrived, and how many were dropped by the apply-time generation re-check.
    int CheckedRefreshLocalDataCalls = 0;
    int CheckedRefreshLocalDataDrops = 0;
    /// Invoked at the top of the checked RefreshLocalData, BEFORE its generation re-check —
    /// models a backend swap landing inside the refresh window (after the worker-side
    /// pre-check, during the full-table cache read, before the under-lock swap-in).
    std::function<void()> OnCheckedRefreshLocalData;
    int DeferredLiveNotifyCalls = 0;
    /// How `LaunchBackgroundTask` should run the task. Default: run inline on the caller.
    /// Tests that want to defer (e.g. to inject a fault between steps) override this.
    std::function<void(std::function<void()>)> BackgroundTaskRunner = [](std::function<void()> t) {
        if (t)
            t();
    };

    /// Cache namespace handed to ticket-cache reads/writes during replay (multi-grid Slice 1b).
    std::string CacheBackendKeyImpl{"Jira"};
    /// Capture-then-check token (issue #1081). Tests bump this mid-replay (via a deferred
    /// `BackgroundTaskRunner`) to model a backend swap between work-capture and apply.
    std::uint64_t BackendGenerationImpl = 0;

    ISyncCache* Cache() override { return CacheImpl.get(); }
    /// Latched strong role handles (debt 2026-06-07): swap-during-replay tests reset
    /// `BackendImpl` mid-replay — the handle a worker captured must keep the old fake alive.
    std::shared_ptr<ITrackerIssueReader> ReaderShared() const override { return BackendImpl; }
    std::shared_ptr<ITrackerIssueMutations> MutationsShared() const override { return BackendImpl; }
    std::string CacheBackendKey() const override { return CacheBackendKeyImpl; }
    std::uint64_t BackendGeneration() const override { return BackendGenerationImpl; }
    const std::vector<TrackerField>& AvailableFields() const override { return Fields; }
    RequiredFieldSet GetRequiredFieldSet(const std::string& /*projectKey*/, const std::string& /*issueTypeId*/,
                                         const std::string& /*issueTypeName*/) const override {
        return Required;
    }
    void LaunchBackgroundTask(std::function<void()> task) override {
        if (BackgroundTaskRunner) {
            BackgroundTaskRunner(std::move(task));
        }
    }
    void RefreshLocalData() override { ++RefreshLocalDataCalls; }
    void RefreshLocalData(std::uint64_t capturedBackendGeneration) override {
        ++CheckedRefreshLocalDataCalls;
        if (OnCheckedRefreshLocalData) {
            OnCheckedRefreshLocalData();
        }
        // Production semantics: re-check the captured generation at apply time (under
        // activeTicketsMutex_ there) and drop the wholesale replace on mismatch.
        if (capturedBackendGeneration == BackendGenerationImpl) {
            ++RefreshLocalDataCalls;
        } else {
            ++CheckedRefreshLocalDataDrops;
        }
    }
    void RequestDeferredLiveTrackerBackendSuccessNotify() override { ++DeferredLiveNotifyCalls; }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_OFFLINE_QUEUE_DEPS_H
