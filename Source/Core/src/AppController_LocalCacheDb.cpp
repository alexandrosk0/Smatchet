// AppController_LocalCacheDb.cpp — local-cache database block extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/appcontroller-clusters-followup.md). The method DECLARATIONS stay in
// AppController.h; only the definitions and the file-local RemoveLocalCacheDbFiles helper
// moved, so linkage and behavior are identical. The helper is used exclusively by
// the cache recreate, so it moves with the cluster in a fresh anonymous namespace.
// This TU never touches the pImpl, so it does not need the companion-TU subsystem superset.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining the AppController local-cache database methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on

#include "ConfigManager.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "Ui/SmatchetToast.h" // the completion toast (the recreate has no other UI)
// The cache recreate drives per-context streaming-sync teardown through
// GridLiveContext::ticketSync_ and clears OfflineQueueService state via offlineQueue_; both are
// member unique_ptrs that AppController.h only forward-declares, so the pointees must be complete
// here for the destructor and method calls.
#include "OfflineQueueService.h"
#include "Sync/PendingActionQueueService.h"
#include "Sync/CacheRecreateReportPure.h"
#include "TicketSyncService.h"

#include <ghc/filesystem.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Deletes the database file and its -wal/-shm sidecars. `anyRemoved` is set as soon as one of them is gone, so a
// caller can tell a wipe that went through partway (a sidecar would not delete) from one that never started.
VoidResult RemoveLocalCacheDbFiles(const std::string& dbPathUtf8, bool& anyRemoved) {
    anyRemoved = false;
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
            if (ec) {
                // A file that cannot be inspected is not known to be gone. Reopening it as the "fresh"
                // database would mark its one-time migrations done without moving its rows.
                return VoidResult::Err("Could not inspect " + f.string() + ": " + ec.message());
            }
            continue;
        }
        fs::remove(f, ec);
        if (ec) {
            return VoidResult::Err("Could not remove " + f.string() + ": " + ec.message());
        }
        anyRemoved = true;
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

VoidResult AppController::StartRecreateLocalCacheDatabase(
    std::function<void(const smatchet::sync::CacheRecreateOutcome&)> onDone) {
    // UI thread. The gate goes up first, so two callers cannot both start; a refusal lowers it again.
    bool expected = false;
    if (!cacheRecreating_.compare_exchange_strong(expected, true)) {
        return VoidResult::Err(smatchet::sync::CacheRecreateStartRefusal(false, false, true));
    }
    const std::string refusal =
        smatchet::sync::CacheRecreateStartRefusal(localCacheDbPath_.empty(), shuttingDown_.load(), false);
    if (!refusal.empty()) {
        cacheRecreating_.store(false);
        return VoidResult::Err(refusal);
    }
    if (cacheRecreateWorker_.joinable()) {
        cacheRecreateWorker_.join(); // the previous run posted its completion as its last step: an instant join
    }

    // DR6: quiesce EVERY live pane's streaming-sync worker, not just the focused one. Each non-focused
    // GridLiveContext runs its own TicketSyncService std::thread that dereferences Cache via the sync-worker path.
    // The services are flagged cancelled, blocked from starting anything new, and hand their worker threads over:
    // the UI thread never joins them (a request on the wire runs to its own timeout), the recreate worker does.
    const std::vector<TicketSyncService*> services = CollectPaneSyncServices_();
    auto syncThreads = std::make_shared<std::vector<std::thread>>();
    for (TicketSyncService* svc : services) {
        std::thread handedOut = svc->BeginCacheTeardown();
        if (handedOut.joinable()) {
            syncThreads->push_back(std::move(handedOut));
        }
    }
    const std::string focusedCacheKey = focusedContext().CacheBackendKeyCopy();
    try {
        cacheRecreateWorker_ = std::thread([this, syncThreads, focusedCacheKey, onDone]() {
            RunCacheRecreateWorker_(syncThreads, focusedCacheKey, onDone);
        });
    } catch (const std::exception& ex) {
        // No worker could start: undo the gate. Joining here is the last resort (the threads were cancelled).
        for (std::thread& t : *syncThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        for (TicketSyncService* svc : services) {
            svc->EndCacheTeardown();
        }
        cacheRecreating_.store(false);
        return VoidResult::Err(std::string("Could not start the cache recreate: ") + ex.what());
    }
    LOG_INFO("AppController: local cache recreate started (%zu pane sync worker(s) to stop).", syncThreads->size());
    return VoidOk();
}

void AppController::DrainCatalogWorkBeforeShutdown_() {
    if (!catalogSnapshotIo_.Flush(std::chrono::seconds(5))) {
        LOG_WARN("AppController shutdown: the field-catalog snapshot save did not finish within 5 s.");
    }
    if (startupCatalogRestoreSlot_.valid()) {
        startupCatalogRestoreSlot_.wait();
    }
}

void AppController::JoinCacheRecreateWorker_() {
    cacheRecreateAbort_.store(true);
    if (cacheRecreateWorker_.joinable()) {
        cacheRecreateWorker_.join();
    }
}

std::vector<TicketSyncService*> AppController::CollectPaneSyncServices_() {
    std::vector<TicketSyncService*> services;
    services.reserve(gridContexts_.size() + 1);
    for (auto& entry : gridContexts_) {
        if (entry.second && entry.second->ticketSync_) {
            services.push_back(entry.second->ticketSync_.get());
        }
    }
    // The focused context may be a retired husk (ADR-0012 graveyard) outside the live map. When it is a live
    // entry instead, the duplicate is harmless: BeginCacheTeardown / EndCacheTeardown are idempotent.
    if (focusedContext().ticketSync_) {
        services.push_back(focusedContext().ticketSync_.get());
    }
    return services;
}

void AppController::RunCacheRecreateWorker_(std::shared_ptr<std::vector<std::thread>> syncThreads,
                                            std::string focusedCacheKey,
                                            std::function<void(const smatchet::sync::CacheRecreateOutcome&)> onDone) {
    // All flagged before any is joined (Start), so the wait is about one in-flight request, not one per pane.
    for (std::thread& t : *syncThreads) {
        if (t.joinable()) {
            t.join();
        }
    }
    syncThreads->clear();
    smatchet::sync::CacheRecreateOutcome outcome;
    try {
        outcome = RecreateCacheFilesOnWorker_(focusedCacheKey);
    } catch (const std::exception& ex) {
        outcome = smatchet::sync::CacheRecreateOutcome();
        outcome.Error = ex.what();
    } catch (...) {
        outcome = smatchet::sync::CacheRecreateOutcome();
        outcome.Error = "unknown error";
    }
    // A completion is never dropped for a full queue; at shutdown the post is a no-op and the process is ending.
    mainThreadDispatcher.PostCompletionToMainThread(
        [this, outcome, onDone]() { FinishCacheRecreate_(outcome, onDone); });
}

bool AppController::WithdrawCacheOnUiThread_(std::shared_ptr<LocalCacheManager>& outOldCache) {
    auto handedBack = std::make_shared<std::promise<std::shared_ptr<LocalCacheManager>>>();
    std::future<std::shared_ptr<LocalCacheManager>> future = handedBack->get_future();
    // atomic_exchange: from this point every reader sees no cache, and the old one is held only by whoever latched it
    // before. Doing it on the UI thread, between frames, means no UI code holds a raw pointer into it (the sync
    // deps hand those out), so it can be closed on this worker without a use-after-free on the UI thread.
    mainThreadDispatcher.PostCompletionToMainThread([this, handedBack]() {
        handedBack->set_value(std::atomic_exchange(&Cache, std::shared_ptr<LocalCacheManager>()));
    });
    handedBack.reset(); // the posted task is now the promise's sole owner: a task the dispatcher drops breaks it
    while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        if (cacheRecreateAbort_.load() || shuttingDown_.load()) {
            return false;
        }
    }
    try {
        outOldCache = future.get();
    } catch (const std::future_error&) {
        return false; // the post was discarded (shutdown)
    }
    return true;
}

smatchet::sync::CacheRecreateOutcome AppController::RecreateCacheFilesOnWorker_(const std::string& focusedCacheKey) {
    smatchet::sync::CacheRecreateOutcome outcome;
    std::shared_ptr<LocalCacheManager> oldCache;
    if (!WithdrawCacheOnUiThread_(oldCache)) {
        outcome.Error = "Application is shutting down.";
        return outcome;
    }
    // Background tasks that latched the old cache before it was withdrawn finish first; the last reference is then
    // dropped here, so SQLite closes on this worker and the files can be deleted.
    JoinBackgroundTasks();
    // Off-thread latchers outside the pool (Lua / MCP binds, offline-queue workers) may still hold it: give them a
    // short bounded while to let go, or the Windows file delete would hit a sharing violation.
    for (int waits = 0; waits < 50 && oldCache.use_count() > 1 && !cacheRecreateAbort_.load(); ++waits) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    oldCache.reset();

    bool anyRemoved = false;
    const VoidResult removeResult = RemoveLocalCacheDbFiles(localCacheDbPath_, anyRemoved);
    outcome.Wiped = anyRemoved;
    if (!removeResult.has_value()) {
        outcome.Error = removeResult.error();
        try {
            std::atomic_store(&Cache, std::make_shared<LocalCacheManager>(localCacheDbPath_));
        } catch (const std::exception& ex) {
            outcome.Error += std::string(" Failed to reopen database: ") + ex.what();
        }
        return outcome;
    }

    outcome.Wiped = true; // every cache file is gone from here on, whatever happens to the new database
    std::shared_ptr<LocalCacheManager> fresh;
    try {
        fresh = std::make_shared<LocalCacheManager>(localCacheDbPath_);
    } catch (const std::exception& ex) {
        outcome.Error = std::string("Failed to open new database: ") + ex.what();
        return outcome;
    }
    try {
        (void)fresh->RunOneTimeLegacyDropPendingAtMaxAttempts();
        // Fresh DB — nothing to copy, but stamp the tickets_v2 migration flag so the one-time
        // sweep never runs against rows written after this point (multi-grid Slice 1b).
        (void)fresh->RunOneTimeTicketsV2CopyMigration(focusedCacheKey);
        // Same for the pending-queue backend_key stamp (Slice 1c) — fresh DB, nothing to stamp,
        // but consume the one-time flag so it never runs against rows written after this point.
        (void)fresh->RunOneTimePendingQueueBackendKeyStamp(focusedCacheKey);
        // And the site-key re-key (#2268): a fresh file holds no legacy rows; an empty list only sets the flag.
        (void)fresh->RunOneTimeCacheIdentityRekey(std::vector<std::pair<std::string, std::string>>());
    } catch (const std::exception& ex) {
        LOG_WARN("AppController::StartRecreateLocalCacheDatabase legacy cleanup: %s", ex.what());
    } catch (...) {
        LOG_WARN("AppController::StartRecreateLocalCacheDatabase legacy cleanup: unknown exception");
    }
    std::atomic_store(&Cache, fresh); // published only once it is fully set up
    outcome.Ok = true;
    return outcome;
}

void AppController::FinishCacheRecreate_(
    const smatchet::sync::CacheRecreateOutcome& outcome,
    const std::function<void(const smatchet::sync::CacheRecreateOutcome&)>& onDone) {
    // The worker threads are joined and the cache is swapped: drop the cancelled sessions' batches and stale-deletion
    // state, and let syncs start again.
    for (TicketSyncService* svc : CollectPaneSyncServices_()) {
        svc->EndCacheTeardown();
    }
    if (outcome.Ok) {
        ClearLastTrackerTicketSyncWarning();
        if (offlineQueue_) {
            offlineQueue_->legacyPendingStartupBanner_.clear();
            offlineQueue_->RequestSnapshotRefresh(); // the queue view must come from the new file
        }
        if (pendingActions_) {
            pendingActions_->RequestSnapshotRefresh(); // the queue view must come from the new file
        }
        RefreshLocalDataAsync();
    } else if (outcome.Wiped) {
        RefreshAfterPartialCacheWipe_();
    }
    if (outcome.Wiped) {
        RefreshOtherPanesAfterCacheWipe_(); // every pane showed rows from the file that is gone, not only the focused
                                            // one
    }
    cacheRecreating_.store(false); // down before onDone, which may start the follow-up sync
    if (outcome.Ok) {
        LOG_INFO("AppController: local cache recreated.");
        SmatchetToastManager::Instance().Push(
            std::string(SmatchetLocalization::T("toast.success", "Success")),
            std::string(
                SmatchetLocalization::T("toast.local_db_recreated", "Local database recreated; refreshing issues.")),
            ToastType::Success, 4000);
    } else {
        const std::string detail = smatchet::sync::DescribeCacheRecreateOutcome(outcome);
        LOG_WARN("AppController: local cache recreate failed: %s", detail.c_str());
        SmatchetToastManager::Instance().Push(
            std::string(SmatchetLocalization::T("toast.local_db_error_title", "Local database")), detail,
            ToastType::Error, 6000);
    }
    if (onDone) {
        onDone(outcome);
    }
}

void AppController::RefreshAfterPartialCacheWipe_() {
    // The cache files are gone but the recreate did not finish: the panes and the queue views must not keep
    // showing rows that no longer exist. A reopened database re-reads empty; with no database at all the focused
    // pane's rows are cleared directly (RefreshLocalDataAsync has nothing to read).
    if (offlineQueue_) {
        offlineQueue_->legacyPendingStartupBanner_.clear();
        offlineQueue_->RequestSnapshotRefresh();
    }
    if (pendingActions_) {
        pendingActions_->RequestSnapshotRefresh();
    }
    if (std::atomic_load(&Cache)) {
        RefreshLocalDataAsync();
        return;
    }
    GridLiveContext& ctx = focusedContext();
    {
        std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
        ctx.ActiveTickets.clear();
        ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>();
    }
    ctx.ActiveTicketsRevision.fetch_add(1);
    NotifyLuaTicketDataChanged();
}

void AppController::RefreshOtherPanesAfterCacheWipe_() {
    const bool haveCache = std::atomic_load(&Cache) != nullptr;
    GridLiveContext* const focused = &focusedContext();
    for (std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.begin();
         it != gridContexts_.end(); ++it) {
        GridLiveContext& ctx = *it->second;
        if (&ctx == focused) {
            continue;
        }
        // Re-arm the pane's first-sync latch (UI thread, like every write of these fields): a visible pane runs
        // EnsurePaneLiveSyncStarted each frame, so it syncs against the new database without being refocused. A pane
        // whose service already replayed a request held back by the gate (opened mid-recreate) is syncing: left alone.
        if (!ctx.ticketSync_ || !ctx.ticketSync_->HasPendingWork()) {
            ctx.initialSyncKicked = false;
            ctx.lastSyncedJql.clear();
            ctx.syncRetryAfter = std::chrono::steady_clock::time_point();
        }
        if (haveCache) {
            GridLiveContext* const ctxPtr = &ctx;
            const std::uint64_t generation = ctx.backendGeneration_.load();
            try {
                LaunchBackgroundTask(
                    [this, ctxPtr, generation]() { RefreshLocalDataCheckedImpl_(*ctxPtr, &generation); });
            } catch (const std::exception& ex) {
                LOG_WARN("AppController: refreshing pane '%s' after the cache recreate did not start: %s",
                         ctx.PaneId.c_str(), ex.what());
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
            ctx.ActiveTickets.clear();
            ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>();
        }
        ctx.ActiveTicketsRevision.fetch_add(1);
    }
}

bool AppController::OpenLocalCacheFileForTest(const std::string& dbPath) {
    try {
        std::atomic_store(&Cache, std::make_shared<LocalCacheManager>(dbPath));
    } catch (const std::exception& ex) {
        LOG_ERROR("OpenLocalCacheFileForTest: failed to open '%s': %s", dbPath.c_str(), ex.what());
        return false;
    }
    localCacheDbPath_ = dbPath;
    // The services that read the cache (edit-meta, offline queues) exist only once wired; the recreate's refresh uses
    // them.
    WireCoreServices();
    return true;
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
        if (ConfigManager::Load().ReadOnlyMode) {
            // Update re-reads under the write lock (#2191), so this cannot revert a concurrent edit.
            ConfigManager::Update([](TrackerConfig& cfg) { cfg.ReadOnlyMode = false; });
            ConfigManager::InvalidateCache();
            LOG_INFO("EnsureLocalCacheForUiTest: cleared first-run ReadOnlyMode default so "
                     "offline-create populated path can activate");
        }
    }
    return Cache != nullptr;
}
