#ifndef SMATCHET_TESTS_FAKE_TICKET_SYNC_DEPS_H
#define SMATCHET_TESTS_FAKE_TICKET_SYNC_DEPS_H

// FakeTicketSyncDeps — header-only in-memory implementation of `ITicketSyncDeps` for the
// doctest rig. Backs the cache with `LocalCacheManager(":memory:")` and the tracker backend
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
#include "LocalCacheManager.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
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
    std::unique_ptr<LocalCacheManager> CacheImpl{std::make_unique<LocalCacheManager>(":memory:")};
    std::unique_ptr<ITrackerBackend> BackendImpl{std::unique_ptr<ITrackerBackend>(new FakeTrackerClient())};
    std::unique_ptr<ITrackerBackendFactory> Factory{
        std::unique_ptr<ITrackerBackendFactory>(new FakeTrackerBackendFactory())};

    std::string LastTrackerTicketSyncWarning;
    ConnectivityState LastConnectivityState = ConnectivityState::Unknown;
    std::chrono::steady_clock::time_point NextProbeAt{};
    std::chrono::steady_clock::time_point LastPushOutageAt{};
    int PushOutageCalls = 0;
    int DeferredLiveNotifyCalls = 0;

    mutable std::mutex ActiveTicketsMutexImpl;
    std::vector<CachedTicket> ActiveTicketsImpl;
    std::shared_ptr<const std::vector<CachedTicket>> ActiveTicketsPublishedImpl;
    int ActiveTicketsRevisionImpl = 0;

    int PruneEditMetaCalls = 0;
    int WarmIssueTypeEditMetaCalls = 0;
    int NotifyLuaCalls = 0;
    bool PendingLuaWindowBumpImpl = false;
    /// Cache namespace handed to every LocalCacheManager ticket call (multi-grid Slice 1b).
    /// Default matches the Jira FakeTrackerClient so SwapBackendIfTrackerChanged's re-stamp
    /// (NormalizeViewsBackendKey) is a no-op for single-backend tests.
    std::string CacheBackendKeyImpl{"Jira"};

    LocalCacheManager* Cache() override { return CacheImpl.get(); }
    ITrackerIssueReader* Backend() override { return BackendImpl ? &BackendImpl->Reader() : nullptr; }
    ITrackerConnectivity* BackendConnectivity() override {
        return BackendImpl ? &BackendImpl->Connectivity() : nullptr;
    }
    void SetBackend(std::unique_ptr<ITrackerBackend> backend) override { BackendImpl = std::move(backend); }
    ITrackerBackendFactory* BackendFactory() override { return Factory.get(); }
    std::string CacheBackendKey() const override { return CacheBackendKeyImpl; }
    void SetCacheBackendKey(const std::string& key) override { CacheBackendKeyImpl = key; }

    void SetLastTrackerTicketSyncWarning(const std::string& message) override {
        LastTrackerTicketSyncWarning = message;
    }
    void SetLastTrackerConnectivityState(ConnectivityState state) override { LastConnectivityState = state; }
    void SetNextTrackerConnectivityProbeAt(std::chrono::steady_clock::time_point at) override { NextProbeAt = at; }
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) override {
        LastPushOutageAt = now;
        ++PushOutageCalls;
    }
    void RequestDeferredLiveTrackerBackendSuccessNotify() override { ++DeferredLiveNotifyCalls; }

    std::mutex& ActiveTicketsMutex() override { return ActiveTicketsMutexImpl; }
    std::vector<CachedTicket>& ActiveTickets() override { return ActiveTicketsImpl; }
    void SetActiveTicketsPublished(std::shared_ptr<const std::vector<CachedTicket>> snap) override {
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
