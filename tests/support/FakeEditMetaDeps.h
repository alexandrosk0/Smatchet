#ifndef SMATCHET_TESTS_FAKE_EDIT_META_DEPS_H
#define SMATCHET_TESTS_FAKE_EDIT_META_DEPS_H

// FakeEditMetaDeps — header-only in-memory implementation of `IEditMetaDeps` for the doctest
// rig. Backs the backend with a `FakeTrackerClient` held by shared_ptr (so BackendShared()
// hands back a latched strong handle, exactly as the production adapter's atomic_load does and
// as the off-thread warm workers require), and keeps the rest of the AppController-side state in
// plain members tests read directly.
//
// This fixture is the test-side counterpart of `GridContextDepsAdapter`. Any new method added
// to `IEditMetaDeps` MUST be implemented here too — the override list mirrors the production
// adapter.
//
// SQLite/ImGui/cpr-free by construction (ADR-0020 sync-cache purity): it pulls only the narrow
// interface headers + FakeTrackerClient. No LocalCacheManager, no SqliteMemFixture.

#include "FakeTrackerClient.h"
#include "IEditMetaDeps.h"
#include "ILookupCache.h"
#include "ITrackerBackend.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet_tests {

class FakeEditMetaDeps : public IEditMetaDeps {
  public:
    /// Latched backend handle. Held by shared_ptr so BackendShared() returns a strong copy the
    /// warm worker can capture (matches the adapter's atomic_load(&ctx_.Backend) contract). The
    /// concrete FakeTrackerClient is reachable via Fake() for editmeta/component scripting.
    std::shared_ptr<ITrackerBackend> BackendImpl{std::make_shared<FakeTrackerClient>()};

    /// Active-tickets snapshot returned by GetActiveTicketsSnapshot() — used by
    /// ResolveIssueTypeKeyForIssue, PruneEditMetaCacheToActiveTickets, and the warm-start scan.
    std::vector<CachedTicket> ActiveTicketsImpl;

    /// Catalog rows FindFieldSnapshotById scans (CanEditFieldForIssue uses this to detect sprint fields).
    std::vector<TrackerField> AvailableFieldsImpl;

    bool ShuttingDownImpl = false;
    mutable int DeferredNotifyCalls = 0;
    TrackerConnectivityState ConnectivityImpl = TrackerConnectivityState::AuthenticatedReachable;
    /// Null by default (no local store, like before the cache exists); tests that exercise the
    /// remembered workflow install a FakeLookupCache.
    std::shared_ptr<ILookupCache> LookupCacheImpl;
    std::string CacheBackendKeyImpl = "Jira";

    /// SteadyNow() answers `NowImpl` while `UseFakeNow` is set (age-limit tests advance it with AdvanceNow), the
    /// real steady clock otherwise. Set both before a worker runs: they are plain members.
    bool UseFakeNow = false;
    std::chrono::steady_clock::time_point NowImpl{};
    void AdvanceNow(std::chrono::seconds by) {
        if (!UseFakeNow) {
            UseFakeNow = true;
            NowImpl = std::chrono::steady_clock::now(); // continue from the real clock, never step back in time
        }
        NowImpl += by;
    }

    /// LaunchBackgroundTask mode. Default INLINE-SYNCHRONOUS (runs the task on the calling
    /// thread immediately) so logic-coverage cases stay deterministic and single-threaded.
    /// Flip RunOnRealThread=true for the TSan race case: the task then runs on a real
    /// std::thread captured in LaunchedThreads (tests join them before fixture teardown).
    bool RunOnRealThread = false;
    /// Destroy every launched task unrun, as AppController::LaunchBackgroundTask does once shutdown has begun.
    /// Wins over RunOnRealThread.
    bool DropTasks = false;
    std::vector<std::thread> LaunchedThreads; ///< guarded by launchMutex_ while tasks can still be launched
    /// Atomic: the real-thread cases launch from several threads at once (warm worker, scope-save drains).
    std::atomic<int> LaunchBackgroundTaskCalls{0};

    ~FakeEditMetaDeps() override { JoinLaunchedThreads(); }

    /// Convenience accessor for the concrete fake backend (editmeta/component scripting).
    FakeTrackerClient* Fake() { return static_cast<FakeTrackerClient*>(BackendImpl.get()); }

    /// Join any real-thread tasks. Tests call this explicitly in the race case before asserting
    /// final state; the destructor also calls it as a backstop.
    void JoinLaunchedThreads() {
        // A joined worker may itself launch another (a scope-save drain): swap out a batch, join it, repeat.
        for (;;) {
            std::vector<std::thread> batch;
            {
                std::lock_guard<std::mutex> lock(launchMutex_);
                batch.swap(LaunchedThreads);
            }
            if (batch.empty()) {
                return;
            }
            for (auto& t : batch) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    }

    // --- IEditMetaDeps overrides --------------------------------------------------------------

    std::shared_ptr<ITrackerBackend> BackendShared() const override { return BackendImpl; }

    std::shared_ptr<ILookupCache> LookupCacheShared() override { return LookupCacheImpl; }

    std::string CacheBackendKey() const override { return CacheBackendKeyImpl; }

    std::chrono::steady_clock::time_point SteadyNow() const override {
        return UseFakeNow ? NowImpl : std::chrono::steady_clock::now();
    }

    void LaunchBackgroundTask(std::function<void()> task) override {
        ++LaunchBackgroundTaskCalls;
        if (DropTasks) {
            return;
        }
        if (RunOnRealThread) {
            std::lock_guard<std::mutex> lock(launchMutex_);
            LaunchedThreads.emplace_back(std::move(task));
        } else {
            task(); // inline-synchronous: deterministic logic coverage
        }
    }

    bool IsShuttingDown() const override { return ShuttingDownImpl; }

    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override {
        return std::make_shared<const std::vector<CachedTicket>>(ActiveTicketsImpl);
    }

    std::shared_ptr<const TrackerField> FindFieldSnapshotById(const std::string& fieldId) const override {
        for (const auto& f : AvailableFieldsImpl) {
            if (f.Id == fieldId) {
                return std::make_shared<const TrackerField>(f);
            }
        }
        return nullptr;
    }

    void RequestDeferredLiveTrackerBackendSuccessNotify() const override { ++DeferredNotifyCalls; }

    TrackerConnectivityState TrackerConnectivity() const override { return ConnectivityImpl; }

  private:
    std::mutex launchMutex_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_EDIT_META_DEPS_H
