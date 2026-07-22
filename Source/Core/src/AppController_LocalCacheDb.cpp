// AppController_LocalCacheDb.cpp — local-cache database block extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/appcontroller-clusters-followup.md). The method DECLARATIONS stay in
// AppController.h; only the definitions and the file-local RemoveLocalCacheDbFiles helper
// moved, so linkage and behavior are identical. The helper is used exclusively by
// RecreateLocalCacheDatabase, so it moves with the cluster in a fresh anonymous namespace.
// This TU never touches the pImpl, so it does not need the companion-TU subsystem superset.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining the AppController local-cache database methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on

#include "ConfigManager.h"
#include "LocalCacheManager.h"
#include "Logger.h"
// RecreateLocalCacheDatabase drives per-context streaming-sync teardown through
// GridLiveContext::ticketSync_ and clears OfflineQueueService state via offlineQueue_; both are
// member unique_ptrs that AppController.h only forward-declares, so the pointees must be complete
// here for the destructor and method calls.
#include "OfflineQueueService.h"
#include "TicketSyncService.h"

#include <ghc/filesystem.hpp>

#include <exception>
#include <memory>
#include <string>
#include <system_error>

namespace {

VoidResult RemoveLocalCacheDbFiles(const std::string& dbPathUtf8) {
    namespace fs = ghc::filesystem;
    std::error_code ec;
    fs::path p(dbPathUtf8);
    if (!p.is_absolute()) {
        p = fs::absolute(p, ec);
        if (ec) {
            return VoidResult::Err("Could not resolve database path: " + ec.message());
        }
    }
    const std::string stem = p.string();
    const fs::path paths[3] = {p, fs::path(stem + "-wal"), fs::path(stem + "-shm")};
    for (const auto& f : paths) {
        ec.clear();
        if (!fs::exists(f, ec)) {
            continue;
        }
        fs::remove(f, ec);
        if (ec) {
            return VoidResult::Err("Could not remove " + f.string() + ": " + ec.message());
        }
    }
    return VoidOk();
}

} // namespace

std::string AppController::GetResolvedLocalCacheDbPath() const {
    if (localCacheDbPath_.empty()) {
        return {};
    }
    namespace fs = ghc::filesystem;
    std::error_code ec;
    fs::path p(localCacheDbPath_);
    if (!p.is_absolute()) {
        p = fs::absolute(p, ec);
        if (ec) {
            return localCacheDbPath_;
        }
    }
    return p.string();
}

VoidResult AppController::RecreateLocalCacheDatabase() {
    if (localCacheDbPath_.empty()) {
        return VoidResult::Err("Local cache database path is not set.");
    }
    if (shuttingDown_.load()) {
        return VoidResult::Err("Application is shutting down.");
    }

    // hasPendingSyncRequest_ was removed by the TicketSyncService Phase 1C extraction
    // (CODE_REVIEW item 11) — pending-sync state now lives on ticketSync_; the
    // cancel-and-join below covers what the flag used to gate.
    CancelAndJoinActiveStreamingSync();
    JoinBackgroundTasks();

    // Streaming-sync teardown lives entirely on TicketSyncService since Phase 1C of the item
    // 11 extraction: the cancel-and-join clears PendingBatches / BackgroundStaleIds /
    // FetchError / Warning / KeepIds; ResetStaleDeletionState clears the stale-delete
    // counters. Both are no-ops if the service was never `Initialize`d.
    //
    // DR6: quiesce EVERY live pane's streaming-sync worker, not just the focused one. Each
    // non-focused GridLiveContext runs its own TicketSyncService std::thread that dereferences
    // Cache via the sync-worker path; those must be joined before the atomic_store swap below or
    // they race the freed cache. The DR6 atomic snapshot covers the Lua / offline-queue readers.
    // gridContexts_ is a UI-thread-owned map and RecreateLocalCacheDatabase runs on the UI
    // thread, so iterating it here is free of concurrent structural mutation. Per context the
    // cancel-and-join is idempotent (a second call finds no active thread).
    for (auto& entry : gridContexts_) {
        if (entry.second && entry.second->ticketSync_) {
            entry.second->ticketSync_->CancelAndJoinActiveStreamingSync();
            entry.second->ticketSync_->ResetStaleDeletionState();
        }
    }
    // Explicit focused pass in case focusedContextPtr_ currently resolves to a retired husk
    // (ADR-0012 graveyard) that is no longer in the live gridContexts_ map. Idempotent with
    // the loop above when focused is a live entry.
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->CancelAndJoinActiveStreamingSync();
        focusedContext().ticketSync_->ResetStaleDeletionState();
    }

    // DR6: swap the cache via atomic_store so an off-thread worker that snapshotted the old
    // cache with atomic_load keeps it alive for the duration of its use instead of racing this
    // teardown. Mirrors the ADR-0012 Backend atomic-swap pattern (GridLiveContext::Backend).
    std::atomic_store(&Cache, std::shared_ptr<LocalCacheManager>());

    VoidResult removeResult = RemoveLocalCacheDbFiles(localCacheDbPath_);
    if (!removeResult.has_value()) {
        try {
            auto fresh = std::make_shared<LocalCacheManager>(localCacheDbPath_);
            std::atomic_store(&Cache, fresh);
        } catch (const std::exception& ex) {
            return VoidResult::Err(removeResult.error() + " Failed to reopen database: " + ex.what());
        }
        return removeResult;
    }

    try {
        auto fresh = std::make_shared<LocalCacheManager>(localCacheDbPath_);
        std::atomic_store(&Cache, fresh);
    } catch (const std::exception& ex) {
        return VoidResult::Err(std::string("Failed to open new database: ") + ex.what());
    }

    try {
        (void)Cache->RunOneTimeLegacyDropPendingAtMaxAttempts();
        // Fresh DB — nothing to copy, but stamp the tickets_v2 migration flag so the one-time
        // sweep never runs against rows written after this point (multi-grid Slice 1b).
        (void)Cache->RunOneTimeTicketsV2CopyMigration(focusedContext().CacheBackendKeyCopy());
        // Same for the pending-queue backend_key stamp (Slice 1c) — fresh DB, nothing to stamp,
        // but consume the one-time flag so it never runs against rows written after this point.
        (void)Cache->RunOneTimePendingQueueBackendKeyStamp(focusedContext().CacheBackendKeyCopy());
    } catch (const std::exception& ex) {
        LOG_WARN("AppController::RecreateLocalCacheDatabase legacy cleanup: %s", ex.what());
    } catch (...) {
        LOG_WARN("AppController::RecreateLocalCacheDatabase legacy cleanup: unknown exception");
    }

    ClearLastTrackerTicketSyncWarning();
    if (offlineQueue_) {
        offlineQueue_->legacyPendingStartupBanner_.clear();
    }
    RefreshLocalData();
    return VoidOk();
}

bool AppController::EnsureLocalCacheForUiTest() {
    // Bucket-E opt-in (SMATCHET_UITEST_WITH_LOCAL_CACHE=1). A normal boot already
    // opened a file-backed cache in InitConfig; only stand one up when it is unset
    // so this is a true no-op in the common case. The in-memory db lives for the
    // AppController's lifetime (torn down with `Cache`), so the scenario's offline
    // writes never touch the developer profile or any file.
    if (!Cache) {
        try {
            // DR6: publish via atomic_store to stay consistent with the off-thread snapshot
            // readers (mirrors the ADR-0012 Backend atomic-swap pattern).
            std::atomic_store(&Cache, std::make_shared<LocalCacheManager>(":memory:"));
            LOG_INFO("EnsureLocalCacheForUiTest: opened throwaway in-memory LocalCacheManager");
        } catch (const std::exception& ex) {
            LOG_ERROR("EnsureLocalCacheForUiTest: failed to open in-memory cache: %s", ex.what());
            return false;
        }
    }
    // Idempotent (`if (!x)` guards inside) — guarantees offlineQueue_ exists so
    // QueueCreateOffline routes to a live cache instead of returning 0.
    WireCoreServices();
    // A fresh bucket-E spawn child has no setup config file, so ConfigManager::Load()
    // applies the first-run safety default ReadOnlyMode=true (ConfigManager.cpp). That
    // default trips the very first guard in OfflineQueueService::QueueCreateOffline
    // (return 0 under read-only), which would keep the case-8 offline-create populated
    // path SKIPping even with a live cache. This opt-in seam already means "this UI test
    // wants to exercise offline writes", so clear read-only + persist + invalidate so the
    // next ConfigManager::Load() inside the guard observes it. Throwaway profile only
    // (SMATCHET_USER_DATA tmpdir under the harness) — never the developer's real config.
    {
        TrackerConfig cfg = ConfigManager::Load();
        if (cfg.ReadOnlyMode) {
            cfg.ReadOnlyMode = false;
            ConfigManager::Save(cfg);
            ConfigManager::InvalidateCache();
            LOG_INFO("EnsureLocalCacheForUiTest: cleared first-run ReadOnlyMode default so "
                     "offline-create populated path can activate");
        }
    }
    return Cache != nullptr;
}
