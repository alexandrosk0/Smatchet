#ifndef SMATCHET_TESTS_FAKE_TICKET_SYNC_DEPS_H
#define SMATCHET_TESTS_FAKE_TICKET_SYNC_DEPS_H

// FakeTicketSyncDeps — header-only in-memory implementation of `ITicketSyncDeps` for the
// doctest rig. Backs the cache with the in-memory `FakeSyncCache` (ADR-0020 — SQLite-free,
// contract-suite-verified against the real LocalCacheManager) and the tracker backend
// with a `FakeTrackerClient`; the connectivity-banner / ActiveTickets / Lua-bump state lives
// in plain members tests can read directly.
//
// This fixture is the test-side counterpart of `AppControllerDepsAdapter`. Any new method
// added to `ITicketSyncDeps` MUST be implemented here too — the override list mirrors the
// production adapter.

#include "ConfigManager.h"
#include "FakeTrackerClient.h"
#include "ITicketSyncDeps.h"
#include "ITrackerBackendFactory.h"
#include "ITrackerConnectivity.h"
#include "FakeSyncCache.h"
#include "ISyncCache.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet_tests {

/// Minimal factory adapter so `BackendFactory()->Create("Plane" | "Jira")` returns a new
/// `FakeTrackerClient` instance. Tests that need scripted-per-backend behaviour can replace
/// this with their own factory.
class FakeTrackerBackendFactory : public ITrackerBackendFactory {
  public:
    std::unique_ptr<ITrackerBackend> Create(const std::string& /*trackerType*/, const TrackerConfig& /*cfg*/) override {
        return std::unique_ptr<ITrackerBackend>(new FakeTrackerClient());
    }
};

class FakeTicketSyncDeps : public ITicketSyncDeps {
  public:
    std::unique_ptr<FakeSyncCache> CacheImpl{std::make_unique<FakeSyncCache>()};
    std::unique_ptr<ITrackerBackend> BackendImpl{std::unique_ptr<ITrackerBackend>(new FakeTrackerClient())};
    std::unique_ptr<ITrackerBackendFactory> Factory{
        std::unique_ptr<ITrackerBackendFactory>(new FakeTrackerBackendFactory())};

    std::string LastTrackerTicketSyncWarning;
    bool LastTrackerTicketSyncWarningTransient = false;
    ConnectivityState LastConnectivityState = ConnectivityState::Unknown;
    std::chrono::steady_clock::time_point NextProbeAt{};
    std::chrono::steady_clock::time_point LastPushOutageAt{};
    int PushOutageCalls = 0;
    int DeferredLiveNotifyCalls = 0;

    /// Records every NotifySyncStatus call (the UI-free toast hook). Tests assert the
    /// title / level sequence instead of poking the real ImGui toast manager.
    struct RecordedSyncNotification {
        std::string Title;
        std::string Message;
        SyncNotifyLevel Level = SyncNotifyLevel::Info;
        int DurationMs = 0;
    };
    std::vector<RecordedSyncNotification> SyncNotifications;

    mutable std::mutex ActiveTicketsMutexImpl;
    std::vector<CachedTicket> ActiveTicketsImpl;
    std::shared_ptr<const std::vector<CachedTicket>> ActiveTicketsPublishedImpl;
    int ActiveTicketsRevisionImpl = 0;
    /// Publish-under-lock recorder (issue #1081): counts SetActiveTicketsPublished calls and
    /// how many of them happened while ActiveTicketsMutexImpl was held. Probed from a helper
    /// thread via try_lock — a SAME-thread try_lock on a std::mutex the thread already holds
    /// is UB, so the probe must run on its own thread. Tests drive the service single-threaded
    /// at publish time, so "held" means "held by the publishing caller".
    int PublishCalls = 0;
    int PublishCallsUnderLock = 0;

    int PruneEditMetaCalls = 0;
    int WarmIssueTypeEditMetaCalls = 0;
    int NotifyLuaCalls = 0;
    bool PendingLuaWindowBumpImpl = false;
    /// Cache namespace handed to every sync-cache ticket call (multi-grid Slice 1b).
    /// Default matches the Jira FakeTrackerClient so SwapBackendIfTrackerChanged's re-stamp
    /// (NormalizeViewsBackendKey) is a no-op for single-backend tests.
    std::string CacheBackendKeyImpl{"Jira"};

    ISyncCache* Cache() override { return CacheImpl.get(); }
    ITrackerIssueReader* Backend() override { return BackendImpl ? &BackendImpl->Reader() : nullptr; }
    ITrackerConnectivity* BackendConnectivity() override {
        return BackendImpl ? &BackendImpl->Connectivity() : nullptr;
    }
    void SetBackend(std::unique_ptr<ITrackerBackend> backend) override { BackendImpl = std::move(backend); }
    ITrackerBackendFactory* BackendFactory() override { return Factory.get(); }
    std::string CacheBackendKey() const override { return CacheBackendKeyImpl; }
    void SetCacheBackendKey(const std::string& key) override { CacheBackendKeyImpl = key; }

    void SetLastTrackerTicketSyncWarning(const std::string& message, bool transient) override {
        LastTrackerTicketSyncWarning = message;
        LastTrackerTicketSyncWarningTransient = transient;
    }
    void SetLastTrackerConnectivityState(ConnectivityState state) override { LastConnectivityState = state; }
    void SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) override { NextProbeAt = at; }
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) override {
        LastPushOutageAt = now;
        ++PushOutageCalls;
    }
    void RequestDeferredLiveTrackerBackendSuccessNotify() override { ++DeferredLiveNotifyCalls; }
    void NotifySyncStatus(const std::string& title, const std::string& message, SyncNotifyLevel level,
                          int durationMs) override {
        SyncNotifications.push_back(RecordedSyncNotification{title, message, level, durationMs});
    }

    std::mutex& ActiveTicketsMutex() override { return ActiveTicketsMutexImpl; }
    std::vector<CachedTicket>& ActiveTickets() override { return ActiveTicketsImpl; }
    void SetActiveTicketsPublished(std::shared_ptr<const std::vector<CachedTicket>> snap) override {
        ++PublishCalls;
        bool heldByCaller = true;
        std::thread probe([this, &heldByCaller]() {
            if (ActiveTicketsMutexImpl.try_lock()) {
                heldByCaller = false;
                ActiveTicketsMutexImpl.unlock();
            }
        });
        probe.join();
        if (heldByCaller) {
            ++PublishCallsUnderLock;
        }
        ActiveTicketsPublishedImpl = std::move(snap);
    }
    void BumpActiveTicketsRevision() override { ++ActiveTicketsRevisionImpl; }

    void PruneEditMetaCacheToActiveTickets() override { ++PruneEditMetaCalls; }
    void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig /*cfg*/) override { ++WarmIssueTypeEditMetaCalls; }
    void NotifyLuaTicketDataChanged() override { ++NotifyLuaCalls; }
    bool GetPendingLuaWindowBump() const override { return PendingLuaWindowBumpImpl; }
    void SetPendingLuaWindowBump(bool value) override { PendingLuaWindowBumpImpl = value; }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_TICKET_SYNC_DEPS_H
