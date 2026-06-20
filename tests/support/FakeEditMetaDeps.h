#ifndef SMATCHET_TESTS_FAKE_EDIT_META_DEPS_H
#define SMATCHET_TESTS_FAKE_EDIT_META_DEPS_H

// FakeEditMetaDeps — header-only in-memory implementation of `IEditMetaDeps` for the doctest
// rig. Backs the backend with a `FakeTrackerClient` held by shared_ptr (so BackendShared()
// hands back a latched strong handle, exactly as the production adapter's atomic_load does and
// as the off-thread warm workers require), exposes a real default-constructed
// `GridContextFieldCatalog` so warm tests can inspect projectComponentOptions_ after a kick
// (#975 proof), and keeps the rest of the AppController-side state in plain members tests read
// directly.
//
// This fixture is the test-side counterpart of `GridContextDepsAdapter`. Any new method added
// to `IEditMetaDeps` MUST be implemented here too — the override list mirrors the production
// adapter.
//
// SQLite/ImGui/cpr-free by construction (ADR-0020 sync-cache purity): it pulls only the narrow
// interface headers + FakeTrackerClient + GridLiveContext (plain structs/atomics/mutex). No
// LocalCacheManager, no SqliteMemFixture.

#include "FakeTrackerClient.h"
#include "GridLiveContext.h" // GridContextFieldCatalog (real, default-constructed CatalogImpl)
#include "IEditMetaDeps.h"
#include "ITrackerBackend.h"

#include <functional>
#include <memory>
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

    /// Catalog rows FindFieldById scans (CanEditFieldForIssue uses this to detect sprint fields).
    std::vector<TrackerField> AvailableFieldsImpl;

    bool ShuttingDownImpl = false;
    mutable int DeferredNotifyCalls = 0;

    /// Real per-context field catalog the #975 kick-time pointer threads through. Warm tests
    /// inspect CatalogImpl.projectComponentOptions_ / projectComponentsInFlight_ after a kick.
    GridContextFieldCatalog CatalogImpl;

    /// LaunchBackgroundTask mode. Default INLINE-SYNCHRONOUS (runs the task on the calling
    /// thread immediately) so logic-coverage cases stay deterministic and single-threaded.
    /// Flip RunOnRealThread=true for the TSan race case: the task then runs on a real
    /// std::thread captured in LaunchedThreads (tests join them before fixture teardown).
    bool RunOnRealThread = false;
    std::vector<std::thread> LaunchedThreads;
    int LaunchBackgroundTaskCalls = 0;

    ~FakeEditMetaDeps() override { JoinLaunchedThreads(); }

    /// Convenience accessor for the concrete fake backend (editmeta/component scripting).
    FakeTrackerClient* Fake() { return static_cast<FakeTrackerClient*>(BackendImpl.get()); }

    /// Join any real-thread tasks. Tests call this explicitly in the race case before asserting
    /// final state; the destructor also calls it as a backstop.
    void JoinLaunchedThreads() {
        for (auto& t : LaunchedThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        LaunchedThreads.clear();
    }

    // --- IEditMetaDeps overrides --------------------------------------------------------------

    std::shared_ptr<ITrackerBackend> BackendShared() const override { return BackendImpl; }

    void LaunchBackgroundTask(std::function<void()> task) override {
        ++LaunchBackgroundTaskCalls;
        if (RunOnRealThread) {
            LaunchedThreads.emplace_back(std::move(task));
        } else {
            task(); // inline-synchronous: deterministic logic coverage
        }
    }

    bool IsShuttingDown() const override { return ShuttingDownImpl; }

    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override {
        return std::make_shared<const std::vector<CachedTicket>>(ActiveTicketsImpl);
    }

    const TrackerField* FindFieldById(const std::string& fieldId) const override {
        for (const auto& f : AvailableFieldsImpl) {
            if (f.Id == fieldId) {
                return &f;
            }
        }
        return nullptr;
    }

    void RequestDeferredLiveTrackerBackendSuccessNotify() const override { ++DeferredNotifyCalls; }

    GridContextFieldCatalog* KickTimeFieldCatalog() override { return &CatalogImpl; }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_EDIT_META_DEPS_H
