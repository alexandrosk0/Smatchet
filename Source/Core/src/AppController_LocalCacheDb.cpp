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
#include "LeftViewHistoryService.h"
// clang-format on

#include "Config/CacheBackendKeyPure.h" // LegacyCacheKeyRekeys
#include "ConfigManager.h"
#include "LocalCacheManager.h"
#include "Persistence/LocalCacheLocationMigration.h"
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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Deletes the database file and its -wal/-shm sidecars. `anyRemoved` is set once the database file itself is
// gone, so a caller can tell a wipe that removed the rows from one that left them in place.
//
// Every file is first moved aside to a `.wipe` name, and a move that fails puts the moved ones back. A file
// SQLite still holds open cannot be moved on Windows, so the usual failure leaves the database, its WAL and
// its rows exactly as they were. Only then are the staged files deleted, the database first: if that fails
// everything is restored. Once it is gone, a sidecar that will not delete stays under its `.wipe` name, where
// a reopen at the original path cannot pick it up, and the wipe reports an error naming it (with
// `anyRemoved` set), since that file can still hold cached data.
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
    std::vector<std::pair<fs::path, fs::path>> staged; // {original, staged}, database first when present
    const auto restore = [&staged]() {
        for (const auto& f : staged) {
            std::error_code rec;
            fs::rename(f.second, f.first, rec);
            if (rec) {
                LOG_WARN("Cache wipe: could not restore %s from %s: %s", f.first.string().c_str(),
                         f.second.string().c_str(), rec.message().c_str());
            }
        }
    };
    for (const auto& f : paths) {
        ec.clear();
        if (!fs::exists(f, ec)) {
            if (ec) {
                // A file that cannot be inspected is not known to be gone. Reopening it as the "fresh"
                // database would mark its one-time migrations done without moving its rows.
                restore();
                return VoidResult::Err("Could not inspect " + f.string() + ": " + ec.message());
            }
            continue;
        }
        const fs::path aside(f.string() + ".wipe");
        fs::rename(f, aside, ec);
        if (ec) {
            restore();
            return VoidResult::Err("Could not remove " + f.string() + ": " + ec.message());
        }
        staged.emplace_back(f, aside);
    }
    std::string leftovers;
    for (std::size_t i = 0; i < staged.size(); ++i) {
        ec.clear();
        fs::remove(staged[i].second, ec);
        if (!ec) {
            if (staged[i].first == p) {
                anyRemoved = true;
            }
            continue;
        }
        if (staged[i].first == p) {
            restore();
            return VoidResult::Err("Could not remove " + p.string() + ": " + ec.message());
        }
        LOG_WARN("Cache wipe: left %s behind (%s); it is no longer at the database's path.",
                 staged[i].second.string().c_str(), ec.message().c_str());
        leftovers += (leftovers.empty() ? "" : ", ") + staged[i].second.string() + " (" + ec.message() + ")";
    }
    if (!leftovers.empty()) {
        return VoidResult::Err("Removed the database but could not remove " + leftovers + ".");
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

void AppController::SetLocalCacheStatus_(const smatchet::persistence::LocalCacheStatus& status, bool notify) {
    {
        std::lock_guard<std::mutex> lock(localCacheStatusMutex_);
        localCacheStatus_ = status;
        localCacheNoticePending_ = notify;
    }
    localCacheState_.store(static_cast<std::uint8_t>(status.State), std::memory_order_release);
}

smatchet::persistence::LocalCacheStatus AppController::GetLocalCacheStatus() const {
    std::lock_guard<std::mutex> lock(localCacheStatusMutex_);
    return localCacheStatus_;
}

smatchet::persistence::CacheEnqueueBlock AppController::LocalCacheEnqueueBlock() const {
    using smatchet::persistence::CacheEnqueueBlock;
    using smatchet::persistence::LocalCacheState;
    const LocalCacheState state = static_cast<LocalCacheState>(localCacheState_.load(std::memory_order_acquire));
    if (state == LocalCacheState::Opening) {
        return CacheEnqueueBlock::Opening;
    }
    if (cacheRecreating_.load()) {
        return CacheEnqueueBlock::Recreating;
    }
    return state == LocalCacheState::SessionOnly ? CacheEnqueueBlock::SessionOnly : CacheEnqueueBlock::None;
}

std::string AppController::TakeLocalCacheStartupNotice() {
    smatchet::persistence::LocalCacheStatus status;
    {
        std::lock_guard<std::mutex> lock(localCacheStatusMutex_);
        if (!localCacheNoticePending_) {
            return std::string();
        }
        localCacheNoticePending_ = false;
        status = localCacheStatus_;
    }
    smatchet::persistence::LocalCacheNoticeStrings strings;
    strings.SessionOnly = SmatchetLocalization::T(
        "cache.notice.session_only",
        "The local database at {path} could not be opened ({error}). Smatchet is using a temporary cache for this "
        "session: edits cannot be saved offline until it opens.");
    strings.Rebuilt = SmatchetLocalization::T(
        "cache.notice.rebuilt",
        "The local database was damaged and has been rebuilt. The damaged copy is kept at {quarantined}.");
    strings.RebuiltSalvaged = SmatchetLocalization::T(
        "cache.notice.rebuilt_salvaged",
        "The local database was damaged and has been rebuilt. {rows} queued item(s) were recovered. The damaged copy "
        "is kept at {quarantined}.");
    strings.RebuiltSalvageIncomplete = SmatchetLocalization::T(
        "cache.notice.rebuilt_salvage_incomplete",
        "The local database was damaged and has been rebuilt. {rows} queued item(s) were recovered, but some queued "
        "changes could not be. The damaged copy is kept at {quarantined}.");
    return smatchet::persistence::DescribeLocalCacheStartupNotice(status, strings);
}

bool AppController::IsLocalCacheOpening() const {
    return localCacheState_.load(std::memory_order_acquire) ==
           static_cast<std::uint8_t>(smatchet::persistence::LocalCacheState::Opening);
}

bool AppController::WaitForLocalCacheOpen(std::chrono::milliseconds timeout) {
    // The completion is a main-thread task, so a caller with no frame loop (the one-shot CLI, a test) pumps it here.
    // Call it outside a main-thread drain: a completion already swapped into the running drain's batch is invisible to
    // the nested pump, so the wait would run to its timeout.
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    while (IsLocalCacheOpening()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        DrainMainThreadOnce();
        if (IsLocalCacheOpening()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return true;
}

void AppController::RunOneTimeCacheMigrations_(LocalCacheManager& cache, const std::string& resolvedCacheKey,
                                               const std::vector<std::pair<std::string, std::string>>& rekeys) {
    try {
        // Legacy rows were necessarily cached against the then-only configured backend.
        (void)cache.RunOneTimeTicketsV2CopyMigration(resolvedCacheKey);
    } catch (const std::exception& ex) {
        // Pillar 3 graceful degradation: a failed copy leaves the flag unset (transactional), so the next launch
        // retries; the session continues with whatever v2 already holds.
        LOG_ERROR("AppController: tickets_v2 copy migration failed: %s", ex.what());
    }
    // Multi-grid Slice 1c: backfill the pending-queue backend_key columns once. Legacy queue rows were necessarily
    // queued against the then-only configured backend.
    try {
        (void)cache.RunOneTimePendingQueueBackendKeyStamp(resolvedCacheKey);
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController: pending-queue backend_key stamp failed: %s", ex.what());
    }
    // #2268: move rows keyed by tracker kind ("Jira", "Plane", ...) to the site-and-account keys the cache now uses,
    // once, after the stamps above and before the first ticket read or replay tick.
    try {
        (void)cache.RunOneTimeCacheIdentityRekey(rekeys);
    } catch (const std::exception& ex) {
        // The transaction leaves the flag unset on failure. Until the next start the legacy rows sit under keys no
        // context uses: hidden and held, never replayed.
        LOG_ERROR("AppController: cache site-key re-key failed: %s", ex.what());
    }
}

// Worker: copies the best older cache file to `target` when `target` does not exist yet (never touching the older
// file) and records the result on `outcome`. Returns the path to open: `target`, or the older file itself when it
// could not be copied just now (this session then borrows it).
std::string AppController::ImportLegacyCacheForOpen_(const std::string& target,
                                                     const std::vector<std::string>& candidates,
                                                     const std::atomic<bool>& abort, StartupCacheOpenOutcome& outcome) {
    if (candidates.empty()) {
        return target;
    }
    smatchet::persistence::LegacyCacheImportOptions importOptions;
    importOptions.Abort = &abort;
    const smatchet::persistence::LegacyCacheImportResult imported =
        smatchet::persistence::ImportLegacyLocalCacheIfNeeded(target, candidates, importOptions);
    outcome.OtherLegacyCaches = imported.OtherCandidates;
    if (!imported.Detail.empty()) {
        LOG_INFO("AppController: local cache location: %s", imported.Detail.c_str());
    }
    switch (imported.Kind) {
    case smatchet::persistence::LegacyCacheImportKind::Imported:
        outcome.ImportedFrom = imported.LegacyPath;
        break;
    case smatchet::persistence::LegacyCacheImportKind::UseLegacyThisSession:
        outcome.UsingLegacyPath = true;
        outcome.LegacyPath = imported.LegacyPath;
        outcome.OpenedPath = imported.OpenPath;
        return imported.OpenPath;
    case smatchet::persistence::LegacyCacheImportKind::Deferred:
        // Nothing could be copied and the target must not be created outside the import lock: this session runs on a
        // memory-only cache (the next start tries again), with the reason shown.
        throw std::runtime_error("the cache could not be moved from its older location yet: " + imported.Detail);
    case smatchet::persistence::LegacyCacheImportKind::NotNeeded:
    case smatchet::persistence::LegacyCacheImportKind::CreatedEmpty:
    case smatchet::persistence::LegacyCacheImportKind::LegacyUnreadable:
        break;
    }
    return target;
}

AppController::StartupCacheOpenOutcome
AppController::OpenStartupCacheOnWorker_(const StartupCacheOpenRequest& request) noexcept {
    /* PILLAR2_WORKER_ONLY */ // est-latency: 50-25000 ms (open, busy wait, migrations, legacy sweeps)
    StartupCacheOpenOutcome outcome;
    outcome.OpenedPath = request.TargetPath;
    try {
        const std::string openPath =
            ImportLegacyCacheForOpen_(request.TargetPath, request.LegacyCandidates, cacheRecreateAbort_, outcome);
        if (cacheRecreateAbort_.load()) {
            outcome.Aborted = true;
            return outcome;
        }
        LocalCacheManager::OpenOptions options;
        options.Abort = &cacheRecreateAbort_;
        // An older location's file borrowed for this session is not ours to repair: a corrupt one is never moved aside.
        options.RebuildCorruptFile = !outcome.UsingLegacyPath;
        std::shared_ptr<LocalCacheManager> cache = std::make_shared<LocalCacheManager>(openPath, options);
        if (cacheRecreateAbort_.load()) {
            outcome.Aborted = true;
            return outcome;
        }
        try {
            outcome.LegacyDropped = cache->RunOneTimeLegacyDropPendingAtMaxAttempts();
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController: legacy pending cleanup failed: %s", ex.what());
        }
        RunOneTimeCacheMigrations_(*cache, request.FocusedCacheKey, request.Rekeys);
        if (offlineQueue_ && !cacheRecreateAbort_.load()) {
            outcome.QueueChangedBySweep = offlineQueue_->RunLegacyProjectSweepOn(*cache, request.ActiveTrackerType);
        }
        if (request.ActiveTrackerType == "Plane" && !cacheRecreateAbort_.load()) {
            try {
                // One-shot: the saved Plane views are checked once for a project scope (logged on the UI thread, where
                // the backend is reachable), then the flag stops it from ever looking again.
                static const std::string kPlaneSweepFlag = "legacy_plane_view_swept_v1";
                if (!cache->HasCacheMetaFlag(kPlaneSweepFlag)) {
                    const PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
                    const auto bucket = disk.Backends.find(ConfigManager::NormalizeViewsBackendKey("Plane"));
                    if (bucket != disk.Backends.end()) {
                        for (const ViewDefinition& view : bucket->second.Views) {
                            outcome.PlaneViewsToCheck.push_back(std::make_pair(view.Name, view.Jql));
                        }
                    }
                    cache->SetCacheMetaFlag(kPlaneSweepFlag);
                }
            } catch (const std::exception& ex) {
                LOG_ERROR("AppController: legacy Plane-view sweep failed: %s", ex.what());
            }
        }
        if (cacheRecreateAbort_.load()) {
            outcome.Aborted = true;
            return outcome;
        }
        outcome.Recovery = cache->RecoveryReport();
        if (!outcome.Recovery.Rebuilt) {
            try {
                (void)cache->ReadLastRecoveryReport(outcome.Recovery); // an earlier launch's rebuild, for Preferences
            } catch (const std::exception& ex) {
                LOG_WARN("AppController: the last recovery record could not be read: %s", ex.what());
            }
        }
        outcome.OpenedCache = std::move(cache);
        outcome.Opened = true;
        return outcome;
    } catch (const std::exception& ex) {
        outcome.Error = ex.what();
    } catch (...) { // catch-all-ok: recorded as the failure reason and degraded to a session-only cache below
        outcome.Error = "unknown exception";
    }
    if (cacheRecreateAbort_.load()) {
        outcome.Aborted = true;
        return outcome;
    }
    LOG_ERROR("AppController: the local cache '%s' could not be opened (%s); continuing with a session-only cache",
              request.TargetPath.c_str(), outcome.Error.c_str());
    // A corrupt file is rebuilt inside the constructor, so a throw here is an environment fault: an unwritable or
    // missing directory, a locked file, or too old an SQLite. Reads and sync still work against a memory-only cache.
    // Durable enqueues are refused with the reason (CacheEnqueueBlock::SessionOnly) instead of being lost at exit.
    outcome.SessionOnly = true;
    outcome.ImportedFrom.clear();
    outcome.UsingLegacyPath = false;
    outcome.LegacyPath.clear();
    outcome.OpenedPath = request.TargetPath;
    try {
        outcome.OpenedCache = std::make_shared<LocalCacheManager>(":memory:");
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController: the session-only cache could not be opened either: %s", ex.what());
        outcome.OpenedCache.reset();
    } catch (...) { // catch-all-ok: no cache at all; every reader null-guards
        LOG_ERROR("AppController: the session-only cache could not be opened either: unknown exception");
        outcome.OpenedCache.reset();
    }
    return outcome;
}

std::shared_ptr<std::vector<std::thread>>
AppController::QuiescePaneSyncsForCacheGate_(std::vector<TicketSyncService*>& outServices) {
    // DR6: quiesce EVERY live pane's streaming-sync worker, not just the focused one. Each non-focused
    // GridLiveContext runs its own TicketSyncService std::thread that dereferences Cache via the sync-worker path.
    // The services are flagged cancelled, blocked from starting anything new, and hand their worker threads over:
    // the UI thread never joins them (a request on the wire runs to its own timeout), a worker does.
    outServices = CollectPaneSyncServices_();
    std::shared_ptr<std::vector<std::thread>> syncThreads = std::make_shared<std::vector<std::thread>>();
    for (TicketSyncService* svc : outServices) {
        std::thread handedOut = svc->BeginCacheTeardown();
        if (handedOut.joinable()) {
            syncThreads->push_back(std::move(handedOut));
        }
    }
    return syncThreads;
}

void AppController::LaunchStartupCacheOpen_(StartupCacheOpenRequest request) {
    // UI thread. The gate stays up from here until FinishStartupCacheOpen_: no sync starts (requests are held), and an
    // enqueue is refused with "still opening".
    if (shuttingDown_.load()) {
        return;
    }
    cacheRecreating_.store(true);
    {
        smatchet::persistence::LocalCacheStatus status = GetLocalCacheStatus();
        status.State = smatchet::persistence::LocalCacheState::Opening;
        status.Path = request.TargetPath;
        status.LastError.clear();
        SetLocalCacheStatus_(status, false);
    }
    if (cacheRecreateWorker_.joinable()) {
        cacheRecreateWorker_.join(); // the previous run posted its completion as its last step: an instant join
    }
    cacheRecreateAbort_.store(false);
    std::vector<TicketSyncService*> services;
    const std::shared_ptr<std::vector<std::thread>> syncThreads = QuiescePaneSyncsForCacheGate_(services);
    const bool isReopen = request.IsReopen;
    try {
        cacheRecreateWorker_ = std::thread([this, syncThreads, isReopen, request]() {
            for (std::thread& t : *syncThreads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            syncThreads->clear();
            StartupCacheOpenOutcome outcome = OpenStartupCacheOnWorker_(request);
            if (outcome.Aborted) {
                return; // shutting down: nothing is published, and the controller's teardown joins this thread
            }
            mainThreadDispatcher.PostCompletionToMainThread(
                [this, outcome, isReopen]() { FinishStartupCacheOpen_(outcome, isReopen); });
        });
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController: the cache open worker could not start (%s); using a session-only cache", ex.what());
        for (std::thread& t : *syncThreads) {
            if (t.joinable()) {
                t.join();
            }
        }
        StartupCacheOpenOutcome outcome;
        outcome.OpenedPath = request.TargetPath;
        outcome.Error = ex.what();
        outcome.SessionOnly = true;
        try {
            outcome.OpenedCache = std::make_shared<LocalCacheManager>(":memory:"); // no file I/O: fine on this thread
        } catch (const std::exception& memEx) {
            LOG_ERROR("AppController: the session-only cache could not be opened either: %s", memEx.what());
        }
        FinishStartupCacheOpen_(std::move(outcome), isReopen);
    }
}

void AppController::FinishStartupCacheOpen_(StartupCacheOpenOutcome outcome, bool isReopen) {
    // UI thread, and no SQLite or file I/O: the worker did all of it.
    if (outcome.Aborted || shuttingDown_.load()) {
        return;
    }
    std::atomic_store(&Cache, outcome.OpenedCache);
    smatchet::persistence::LocalCacheStatus status = GetLocalCacheStatus();
    status.Path = outcome.OpenedPath;
    status.State = outcome.SessionOnly ? smatchet::persistence::LocalCacheState::SessionOnly
                                       : smatchet::persistence::LocalCacheState::Ready;
    status.LastError = outcome.SessionOnly ? outcome.Error : std::string();
    status.UsingLegacyPath = outcome.UsingLegacyPath;
    status.LegacyPath = outcome.LegacyPath;
    status.ImportedFrom = outcome.ImportedFrom;
    status.OtherLegacyCaches = outcome.OtherLegacyCaches;
    status.LastRecovery = outcome.Recovery;
    // A session-only start and a rebuild of this launch are told to the user once; a recovery from an earlier launch is
    // only shown in Preferences.
    SetLocalCacheStatus_(status, outcome.SessionOnly || (outcome.Recovery.Rebuilt && outcome.Recovery.FromThisLaunch));
    localCacheOpenGeneration_.fetch_add(1,
                                        std::memory_order_acq_rel); // after the status: a reader that sees it sees it

    if (outcome.LegacyDropped > 0 && offlineQueue_) {
        char buf[384];
        std::snprintf(buf, sizeof(buf),
                      "Startup: dropped %zu legacy offline pending row(s) already at max retries "
                      "(not archived). They were removed from the active queue only.",
                      outcome.LegacyDropped);
        offlineQueue_->legacyPendingStartupBanner_ = buf;
    }
    ITrackerBackend* const backend = focusedContext().Backend.get();
    if (backend != nullptr) {
        for (std::size_t i = 0; i < outcome.PlaneViewsToCheck.size(); ++i) {
            if (backend->Connectivity().ExtractProjectFromQuery(outcome.PlaneViewsToCheck[i].second).empty()) {
                LOG_WARN("Plane view '%s' has no project scope; pick a project in the view editor",
                         outcome.PlaneViewsToCheck[i].first.c_str());
            }
        }
    }

    // The queue views and every pane's rows come from the cache that is now there.
    if (offlineQueue_) {
        offlineQueue_->RequestSnapshotRefresh();
    }
    if (pendingActions_) {
        pendingActions_->RequestSnapshotRefresh();
    }
    if (leftViewHistory_) {
        if (isReopen) {
            leftViewHistory_->ResetForCacheRecreate(); // the rows on screen came from the memory-only cache
        } else {
            leftViewHistory_->Load(); // the load at init found no cache yet: read the history now that it is there
        }
    }
    RefreshLocalDataAsync();
    RereadOtherPanesFromCache_();
    // The saved user roster is read once per launch by the UI, on its first draw, which is before this open published
    // the cache: an offline start would show no roster for the whole session. Seed it now (a live roster is never
    // overridden).
    SeedAvailableUsersFromStoreAsync();

    const TicketSyncService* const focusedSync = focusedContext().ticketSync_.get();
    for (TicketSyncService* svc : CollectPaneSyncServices_()) {
        // After a successful retry the focused pane's held request rides on the full sync started below; a retry that
        // failed again starts no sync, so nothing is folded.
        svc->EndCacheTeardown(isReopen && outcome.Opened && svc == focusedSync);
    }
    cacheRecreating_.store(false); // down before the follow-up sync starts
    if (isReopen && outcome.Opened) {
        LOG_INFO("AppController: the local cache is open on its file again.");
        SyncWithBackendMode(nullptr, nullptr, smatchet::sync::SyncMode::Full);
    }
}

VoidResult AppController::StartReopenLocalCache() {
    // UI thread.
    if (shuttingDown_.load()) {
        return VoidResult::Err("Application is shutting down.");
    }
    if (localCacheState_.load(std::memory_order_acquire) !=
        static_cast<std::uint8_t>(smatchet::persistence::LocalCacheState::SessionOnly)) {
        return VoidResult::Err("The local cache is already open.");
    }
    bool expected = false;
    if (!cacheRecreating_.compare_exchange_strong(expected, true)) {
        return VoidResult::Err("The local cache is busy; try again in a moment.");
    }
    StartupCacheOpenRequest request;
    request.IsReopen = true;
    request.TargetPath = localCacheDbPath_;
    request.LegacyCandidates = legacyCacheCandidates_;
    request.FocusedCacheKey = focusedContext().CacheBackendKeyCopy();
    request.Rekeys = smatchet::cache_keys::LegacyCacheKeyRekeys(ConfigManager::Load());
    ITrackerBackend* const backend = focusedContext().Backend.get();
    request.ActiveTrackerType =
        backend != nullptr ? backend->Connectivity().GetTrackerType() : ConfigManager::Load().TrackerType;
    LaunchStartupCacheOpen_(std::move(request));
    return VoidOk();
}

void AppController::StartStartupCacheOpenForTest(const std::string& dbPath, const std::string& cacheKey,
                                                 const std::vector<std::string>& legacyCandidates) {
    localCacheDbPath_ = dbPath;
    legacyCacheCandidates_ = legacyCandidates;
    WireCoreServices();
    initLeftViewHistory_(); // as InitConfig does: its first load runs while the cache is still opening
    focusedContext().SetCacheBackendKey(cacheKey);
    StartupCacheOpenRequest request;
    request.TargetPath = dbPath;
    request.LegacyCandidates = legacyCandidates;
    request.FocusedCacheKey = cacheKey;
    LaunchStartupCacheOpen_(std::move(request));
}

VoidResult AppController::StartRecreateLocalCacheDatabase(
    std::function<void(const smatchet::sync::CacheRecreateOutcome&)> onDone) {
    // UI thread. While the startup open runs, the gate is already up for it: say so instead of "already recreating".
    if (IsLocalCacheOpening()) {
        return VoidResult::Err(smatchet::sync::CacheRecreateStartRefusal(false, false, false, true, true));
    }
    // The gate goes up first, so two callers cannot both start; a refusal lowers it again.
    bool expected = false;
    if (!cacheRecreating_.compare_exchange_strong(expected, true)) {
        return VoidResult::Err(smatchet::sync::CacheRecreateStartRefusal(false, false, true));
    }
    const std::string refusal = smatchet::sync::CacheRecreateStartRefusal(
        localCacheDbPath_.empty(), shuttingDown_.load(), false,
        localCacheState_.load(std::memory_order_acquire) !=
            static_cast<std::uint8_t>(smatchet::persistence::LocalCacheState::Ready),
        IsLocalCacheOpening(), GetLocalCacheStatus().UsingLegacyPath);
    if (!refusal.empty()) {
        cacheRecreating_.store(false);
        return VoidResult::Err(refusal);
    }
    if (cacheRecreateWorker_.joinable()) {
        cacheRecreateWorker_.join(); // the previous run posted its completion as its last step: an instant join
    }

    // The recreate worker joins the sync threads handed out here (the UI thread never does).
    std::vector<TicketSyncService*> services;
    const std::shared_ptr<std::vector<std::thread>> syncThreads = QuiescePaneSyncsForCacheGate_(services);
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

void AppController::FlushTicketSavesBeforeShutdown_() {
    // Once shuttingDown_ is set, LaunchBackgroundTask drops the queue's drain task, so a save posted just before
    // quit would never reach SQLite. Bounded like the catalog flush above.
    if (!ticketSaves_.Flush(std::chrono::seconds(5))) {
        LOG_WARN("AppController shutdown: a queued ticket cache save did not finish within 5 s.");
    }
    FlushLeftViewHistoryBeforeShutdown_(); // the same risk: its queue's drain is a background task too
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

bool AppController::ReleaseWithdrawnCache_(std::shared_ptr<LocalCacheManager> oldCache, std::string& outError) {
    if (!oldCache) {
        return true; // there was no cache open: nothing holds the files
    }
    // The files may be deleted only once the old LocalCacheManager is destroyed and SQLite has closed them. A holder
    // still running (a Lua / MCP bind, a chat persist write, a queued save that locked its weak_ptr) keeps the handle
    // open: deleting under it fails on Windows with a sharing violation, and on POSIX unlinks the file under the live
    // handle, so that holder's commits land in a file nothing reads again, and its later close can checkpoint and
    // delete the -wal / -shm files the new database has opened at the same paths.
    const std::chrono::milliseconds budget((std::max)(0LL, cacheRecreateHolderWaitMs_.load()));
    const std::shared_ptr<LocalCacheCloseSignal> closed = oldCache->CloseSignal();
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + budget;
    while (oldCache.use_count() > 1 && !cacheRecreateAbort_.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const long holdersAtDeadline = oldCache.use_count();
    const std::weak_ptr<LocalCacheManager> oldCacheWeak = oldCache;
    oldCache.reset();
    if (const std::shared_ptr<LocalCacheManager> stillHeld = oldCacheWeak.lock()) {
        // Still in use: keep it, report the recreate as failed with nothing wiped; the user retries once it lets go.
        LOG_WARN("AppController::StartRecreateLocalCacheDatabase: the old cache still had %ld other owner(s) after the "
                 "bounded wait; nothing was deleted and the old cache is back in place.",
                 holdersAtDeadline - 1);
        std::atomic_store(&Cache, stillHeld);
        outError = "The local cache is still in use by a background task. Nothing was deleted; try again in a moment.";
        return false;
    }
    // No owner is left, but if one let go after the wait its thread runs the destructor, not this one: wait for that
    // close to finish (already done when this thread dropped the last reference). Shutdown stops the wait.
    const std::chrono::steady_clock::time_point closeDeadline = std::chrono::steady_clock::now() + budget;
    bool closedInTime = closed->WaitFor(std::chrono::milliseconds(0));
    while (!closedInTime && !cacheRecreateAbort_.load() && std::chrono::steady_clock::now() < closeDeadline) {
        closedInTime = closed->WaitFor(std::chrono::milliseconds(50));
    }
    if (closedInTime) {
        return true;
    }
    if (cacheRecreateAbort_.load()) {
        outError = "Application is shutting down."; // nothing deleted, and no reopen while the app tears down
        return false;
    }
    LOG_WARN("AppController::StartRecreateLocalCacheDatabase: the old cache did not finish closing in time; nothing "
             "was deleted and the file is reopened.");
    outError = "The local cache is still closing. Nothing was deleted; try again in a moment.";
    try {
        std::atomic_store(&Cache, std::make_shared<LocalCacheManager>(localCacheDbPath_));
    } catch (const std::exception& ex) {
        outError += std::string(" Failed to reopen database: ") + ex.what();
    }
    return false;
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
    if (!ReleaseWithdrawnCache_(std::move(oldCache), outcome.Error)) {
        return outcome; // nothing deleted; a cache is back in `Cache`
    }

    bool anyRemoved = false;
    const VoidResult removeResult = RemoveLocalCacheDbFiles(localCacheDbPath_, anyRemoved);
    outcome.Wiped = anyRemoved;
    if (!removeResult.has_value() && !anyRemoved) {
        outcome.Error = removeResult.error();
        try {
            std::atomic_store(&Cache, std::make_shared<LocalCacheManager>(localCacheDbPath_));
        } catch (const std::exception& ex) {
            outcome.Error += std::string(" Failed to reopen database: ") + ex.what();
        }
        return outcome;
    }

    outcome.Wiped = true; // the database is gone from here on, whatever happens to the new one
    // The database went but a sidecar would not: the new database still gets its one-time stamps below (an
    // unstamped fresh file would run those migrations against rows written later), and the leftover is reported.
    const std::string leftover = removeResult.has_value() ? std::string() : removeResult.error();
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
    outcome.Warning = leftover;
    return outcome;
}

void AppController::FinishCacheRecreate_(
    const smatchet::sync::CacheRecreateOutcome& outcome,
    const std::function<void(const smatchet::sync::CacheRecreateOutcome&)>& onDone) {
    // The worker threads are joined and the cache is swapped: drop the cancelled sessions' batches and stale-deletion
    // state, and let syncs start again.
    // On success this function itself starts the full re-fetch of the focused pane (below), so the focused pane's
    // held request rides on that follow-up instead of being replayed first: one session runs, not a replay the
    // follow-up then cancels or duplicates. Every other pane replays its held request as before.
    const bool followUpSync = outcome.Ok;
    const bool foldFocused = followUpSync;
    const TicketSyncService* const focusedSync = focusedContext().ticketSync_.get();
    for (TicketSyncService* svc : CollectPaneSyncServices_()) {
        svc->EndCacheTeardown(foldFocused && svc == focusedSync);
    }
    if (leftViewHistory_ && (outcome.Ok || outcome.Wiped)) {
        leftViewHistory_->ResetForCacheRecreate(); // the rows on screen came from the file that is gone
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
    } else {
        // Nothing was deleted and a cache is back in place, but ticket saves queued before the withdraw were dropped
        // while it was out, so a pane can show rows SQLite never stored: re-read every pane from the file.
        RefreshLocalDataAsync();
        RereadOtherPanesFromCache_();
    }
    if (outcome.Wiped) {
        RefreshOtherPanesAfterCacheWipe_(); // every pane showed rows from the file that is gone, not only the focused
                                            // one
    }
    cacheRecreating_.store(false); // down before the follow-up sync starts
    if (outcome.Ok && !outcome.Warning.empty()) {
        LOG_WARN("AppController: local cache recreated, with a leftover file: %s", outcome.Warning.c_str());
        SmatchetToastManager::Instance().Push(
            std::string(SmatchetLocalization::T("toast.local_db_error_title", "Local database")),
            std::string(SmatchetLocalization::T("toast.local_db_recreated_leftover",
                                                "Local database recreated; refreshing issues. ")) +
                outcome.Warning,
            ToastType::Warning, 6000);
    } else if (outcome.Ok) {
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
    if (followUpSync) {
        // The fold above and this sync are one decision: the focused service holds the folded mode until this call
        // consumes it. The configuration and the views come from disk (the recreate is not a settings save).
        SyncWithBackendMode(nullptr, nullptr, smatchet::sync::SyncMode::Full);
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

void AppController::LaunchPaneReread_(GridLiveContext& ctx) {
    GridLiveContext* const ctxPtr = &ctx;
    const std::uint64_t generation = ctx.backendGeneration_.load();
    try {
        LaunchBackgroundTask([this, ctxPtr, generation]() { RefreshLocalDataCheckedImpl_(*ctxPtr, &generation); });
    } catch (const std::exception& ex) {
        LOG_WARN("AppController: re-reading pane '%s' from the local cache did not start: %s", ctx.PaneId.c_str(),
                 ex.what());
    }
}

void AppController::RereadOtherPanesFromCache_() {
    GridLiveContext* const focused = &focusedContext();
    for (std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.begin();
         it != gridContexts_.end(); ++it) {
        if (it->second && it->second.get() != focused) {
            LaunchPaneReread_(*it->second);
        }
    }
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
            LaunchPaneReread_(ctx);
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

bool AppController::MarkFieldEditConflictForTest(std::int64_t id, const std::string& contextJson) {
    const std::shared_ptr<LocalCacheManager> cache = std::atomic_load(&Cache);
    if (!cache || !offlineQueue_) {
        return false;
    }
    try {
        cache->MarkFieldEditConflict(id, contextJson);
    } catch (const std::exception& ex) {
        LOG_ERROR("MarkFieldEditConflictForTest: id=%lld err=%s", static_cast<long long>(id), ex.what());
        return false;
    }
    offlineQueue_->RequestSnapshotRefresh();
    return true;
}

bool AppController::EnsureLocalCacheForUiTest() {
    // Bucket-E opt-in (SMATCHET_UITEST_WITH_LOCAL_CACHE=1). The startup open runs on a worker: wait for it first, or
    // its cache would replace the throwaway one below. A normal boot already
    // opened a file-backed cache in InitBackends; only stand one up when it is unset
    // so this is a true no-op in the common case. The in-memory db lives for the
    // AppController's lifetime (torn down with `Cache`), so the scenario's offline
    // writes never touch the developer profile or any file.
    if (IsLocalCacheOpening() && !WaitForLocalCacheOpen(std::chrono::seconds(30))) {
        LOG_WARN("EnsureLocalCacheForUiTest: the local cache did not finish opening in 30 s");
    }
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
