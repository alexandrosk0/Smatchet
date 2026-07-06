// winsock2.h must be the FIRST Win32-related include in this TU. Several headers below
// (ghc::filesystem, ConfigManager.h on Windows, ImGui backends) transitively pull
// <windows.h>, which auto-includes the legacy <winsock.h> unless WIN32_LEAN_AND_MEAN is
// defined. cpr/curl (via TrackerHttpUtils.h) then includes <winsock2.h>, which fires
// `#warning Please include winsock2.h before windows.h` from MinGW's headers. Putting the
// winsock2 block at the very top avoids both the warning and the symbol-conflict it
// foreshadows.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include "AppController.h"
#include "AppControllerImpl.h"
#include "GridContextDepsAdapter.h"
#include "GridPaneEvictionPolicy.h"
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls Cache-> methods.
#include <nlohmann/json.hpp> // direct: AppController.h dropped json.hpp for json_fwd (fan-in Phase 1); this TU constructs nlohmann::json.

#include <algorithm>
#include <array>

#include <chrono>

#include <cctype>

#include <cstdint>

#include <cerrno>

#include <cstdio>

#include <cstdlib>

#include <ctime>

#include <exception>

#include <fstream>

#include <mutex>
#include <sstream>

#include <string>

#include <thread>

#include <unordered_set>

#include <utility>

#include <vector>

#include "BackendAuditTrail.h"
#include "BackgroundTaskFirewall.h"
#include "BackgroundWorkerReap.h"
#include "ConfigManager.h"

#include "Commands/BuiltinCommands.h"

#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/SmatchetScenarioRegistry.h"

#include "FieldCatalogCache.h"

#include "Ui/SmatchetFieldRender.h"

#include <ghc/filesystem.hpp>

#include "DefaultTrackerBackendFactory.h"

#include "GitHubFixtureBackend.h"

#include "ITrackerBackendFactory.h"
#include "ITrackerIssueMutations.h"

#include "LinearFixtureBackend.h"

#include "PlaneFixtureBackend.h"

#include "LuaAutomationHost.h"

#include "OfflineQueueService.h"

#include "EditMetaCacheService.h"
#include "FieldEditPipelineService.h"
#include "ConnectivityMonitorService.h"
#include "AttachmentAppUpdateService.h"

#include "PaneSyncKickPolicy.h"

#include "SalientRosterResolve.h"
#include "Sync/MembershipDiffPure.h"
#include "Sync/TicketChangeDiffPure.h"
#include "SmatchetTicketChangeNotifications.h"

#include "TicketSyncService.h"

#include "TrackerHttpUtils.h"

#include "JqlProjectScope.h"
#include "Logger.h"

#include "StringUtil.h"
#include "UiThreadAffinity.h"

#include "Views.h"

#include "SmatchetUI.h"
#include "UiPerfMonitor.h" // SMATCHET_UI_PERF_SCOPE for TickAllContexts (multi-grid concurrent-sync perf scope)

#include "SmatchetToast.h"
#include "SmatchetMergeWatchNotifyServer.h"

#include "AiTypes.h"
#include "ConfigSaveWorker.h" // not AI-gated — config saves happen regardless of feature flags
#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#include "AiAssistantUiStateAdapter.h"
#include "SmatchetChatPersistWorker.h"
#include "SmatchetUiSession.h"
#endif

#if defined(_WIN32)

#include <windows.h>

#include <shellapi.h>

#elif defined(__APPLE__) || defined(__linux__)

#include <unistd.h>

#endif

namespace {

#if defined(__APPLE__) || defined(__linux__)

bool LaunchCommandNoShell(const char* exe, const std::string& arg) {

    if (!exe || arg.empty()) {

        return false;
    }

    const pid_t child = fork();

    if (child < 0) {

        return false;
    }

    if (child == 0) {

        execlp(exe, exe, arg.c_str(), static_cast<char*>(nullptr));

        _exit(127);
    }

    return true;
}

#endif

bool RemoveLocalCacheDbFiles(const std::string& dbPathUtf8, std::string& outError) {
    namespace fs = ghc::filesystem;
    std::error_code ec;
    fs::path p(dbPathUtf8);
    if (!p.is_absolute()) {
        p = fs::absolute(p, ec);
        if (ec) {
            outError = "Could not resolve database path: " + ec.message();
            return false;
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
            outError = "Could not remove " + f.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace

namespace {

std::mutex g_TrackerIssueFetchMutex;

bool FieldIconHasCaseInsensitivePrefix(const std::string& value, const std::string& prefix) {

    if (prefix.empty() || value.size() < prefix.size()) {

        return false;
    }

    for (size_t i = 0; i < prefix.size(); ++i) {

        unsigned char a = static_cast<unsigned char>(value[i]);

        unsigned char b = static_cast<unsigned char>(prefix[i]);

        if (a >= 'A' && a <= 'Z') {

            a = static_cast<unsigned char>(a - 'A' + 'a');
        }

        if (b >= 'A' && b <= 'Z') {

            b = static_cast<unsigned char>(b - 'A' + 'a');
        }

        if (a != b) {

            return false;
        }
    }

    if (value.size() == prefix.size()) {

        return true;
    }

    const char next = value[prefix.size()];

    return next == '/' || next == '\\';
}

// True when absStr is inside the Lua scripts directory or the runtime-asset directory.
// Both roots are weakly-canonicalised before the case-insensitive prefix compare.
bool FieldIconPathIsAllowed(const std::string& absStr, const std::string& luaScriptsDirectory) {
    namespace fs = ghc::filesystem;
    std::error_code ec;

    if (!luaScriptsDirectory.empty()) {

        const fs::path scriptsRoot = fs::weakly_canonical(fs::path(luaScriptsDirectory), ec);

        if (!ec && FieldIconHasCaseInsensitivePrefix(absStr, scriptsRoot.string())) {

            return true;
        }

        ec.clear();
    }

    const std::string base = ConfigManager::GetRuntimeAssetDirectory();

    if (!base.empty()) {

        const fs::path baseRoot = fs::weakly_canonical(fs::path(base), ec);

        if (!ec && FieldIconHasCaseInsensitivePrefix(absStr, baseRoot.string())) {

            return true;
        }

        ec.clear();
    }

    return false;
}

} // namespace

void AppController::SetBackendFactory(std::unique_ptr<ITrackerBackendFactory> factory) {
    backendFactory_ = std::move(factory);
}

// Out-of-line definition (ODR-use in map lookups; C++14).
const std::string AppController::kDefaultPaneId = "main";

namespace {
/// Hidden-pane grace window before a non-default context is retired (multi-grid Slice 3,
/// plan item 17). Long enough that tab-flipping never churns contexts; short enough that a
/// pane parked behind another all session frees its sync worker + ticket memory.
const std::chrono::milliseconds kHiddenContextGrace(30000);
} // namespace

// pImpl (hardening #19): COLD, sol2-/subsystem-heavy state moved off AppController.h so the
// header no longer needs <sol/sol.hpp>. `struct AppController::Impl` is now defined in the
// src-only AppControllerImpl.h (included above) where its sol2 / subsystem member types are
// complete. The out-of-line ctor/dtor here keep the incomplete-type unique_ptr<Impl> in the
// header legal.
AppController::AppController()
    : automationShutdownCancel_(std::make_shared<std::atomic<bool>>(false)), impl_(std::make_unique<Impl>(*this)) {
    // Multi-grid (ADR-0018): the default context is created here — not lazily — and is
    // PERMANENT, so focusedContext()'s fallback stays valid for the controller's entire
    // lifetime (delegators, the deps adapter, and the destructor all assume it exists).
    gridContexts_[kDefaultPaneId] = std::make_unique<GridLiveContext>();
    focusedPaneId_ = kDefaultPaneId;
    focusedContextPtr_ = gridContexts_.find(kDefaultPaneId)->second.get();
}

// Sol-free accessor to the Lua binding host. AppController::Impl implements ILuaBindingHost in
// the Lua build; returns nullptr in the no-Lua build (Impl has no such base there). Keeps sol2
// out of AppController.h — hardening #19c. (ILuaBindingHost is complete here via AppControllerImpl.h.)
ILuaBindingHost* AppController::GetLuaBindingHost() {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    return impl_.get();
#else
    return nullptr;
#endif
}

void AppController::refreshFocusedContextPtr_() {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(focusedPaneId_);
    if (it == gridContexts_.end()) {
        it = gridContexts_.find(kDefaultPaneId); // permanent — always present
    }
    focusedContextPtr_ = it->second.get();
}

void AppController::SetFocusedPane(const std::string& paneId) {
    if (paneId == focusedPaneId_) {
        return;
    }
    focusedPaneId_ = paneId;
    refreshFocusedContextPtr_();
}

GridLiveContext* AppController::EnsurePaneContextLive(const std::string& paneId, const std::string& backendKey) {
    // Issue #1457: hold the map mutex across the resolve-or-emplace so the User Info worker's
    // snapshot-find cannot observe a half-rebalanced tree. UI thread only, so the body is the
    // sole writer; the lock only excludes the worker's brief snapshot read. O(log n) map ops plus
    // a bounded same-backend catalog copy under the lock — no I/O, no Pillar-2 block.
    std::lock_guard<std::mutex> mapLk(gridContextsMutex_);
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        std::unique_ptr<GridLiveContext> ctx = std::make_unique<GridLiveContext>();
        std::unique_ptr<GridContextDepsAdapter> adapter = std::make_unique<GridContextDepsAdapter>(*this, *ctx);
        ctx->ticketSync_ = std::make_unique<TicketSyncService>(*adapter);
        if (!backendKey.empty()) {
            ctx->SetCacheBackendKey(backendKey); // namespaced cache writes before the first swap re-stamps
        }
        // Same-backend seed: copy the default context's catalog so a duplicated / same-backend
        // pane renders dropdown-eligible cells immediately (one-time, pane-show — off any per
        // cell or steady-state path). Cross-backend panes start empty and fill via the focused
        // backend-switch catalog refetch.
        GridLiveContext& defaultCtx = *gridContexts_.find(kDefaultPaneId)->second;
        if (!backendKey.empty() && backendKey == defaultCtx.CacheBackendKeyCopy()) {
            std::lock_guard<std::mutex> srcLock(defaultCtx.fieldCatalog.availableFieldsMutex_);
            ctx->fieldCatalog.AvailableFields = defaultCtx.fieldCatalog.AvailableFields;
            ctx->fieldCatalog.AvailableComponents = defaultCtx.fieldCatalog.AvailableComponents;
            ctx->fieldCatalog.AvailableIssueTypeMeta = defaultCtx.fieldCatalog.AvailableIssueTypeMeta;
            ctx->fieldCatalog.AvailableUsers = defaultCtx.fieldCatalog.AvailableUsers;
            ctx->fieldCatalog.projectComponentOptions_ = defaultCtx.fieldCatalog.projectComponentOptions_;
            ctx->fieldCatalog.fieldCatalogEverLoaded_ = defaultCtx.fieldCatalog.fieldCatalogEverLoaded_;
            ctx->fieldCatalog.currentCatalogProjectKey_ = defaultCtx.fieldCatalog.currentCatalogProjectKey_;
            ctx->fieldCatalog.TrackerFieldCatalogRevision.fetch_add(1);
        }
        paneAdapters_[paneId] = std::move(adapter);
        it = gridContexts_.emplace(paneId, std::move(ctx)).first;
        LOG_INFO("AppController: spun up GridLiveContext for pane '%s' backendKey='%s' (%zu live)", paneId.c_str(),
                 backendKey.c_str(), gridContexts_.size());
        if (paneId == focusedPaneId_) {
            refreshFocusedContextPtr_();
        }
    }
    it->second->lastVisibleAt = std::chrono::steady_clock::now();
    it->second->everVisible = true;
    // LRU recency stamp (multi-grid Slice 5a): bump every frame the pane is visible so the
    // hidden-pane memory cap evicts in reverse visibility order. UI thread only.
    it->second->lastVisibleOrder = ++paneVisibilityClock_;
    // Frame-recency stamp for the cap's visible/hidden classification — FPS-independent
    // (the wall-clock lastVisibleAt above is kept only for the 30 s retirement grace).
    it->second->lastVisibleFrame = paneFrameClock_;
    return it->second.get();
}

void AppController::EnsurePaneLiveSyncStarted(const std::string& paneId, const TrackerConfig& paneCfg,
                                              const std::string& viewId) {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return;
    }
    // Storm damping (issue #1081): a fast-failing backend re-armed initialSyncKicked every
    // failed session, and this per-frame kick site re-launched a full sync at frame rate.
    // The deps session-end hook opens a 30 s retry window (syncRetryAfter) — bail inside it.
    if (!smatchet::ShouldKickInitialSync(std::chrono::steady_clock::now(), it->second->initialSyncKicked,
                                         it->second->syncRetryAfter)) {
        return;
    }
    it->second->initialSyncKicked = true;
    // Capture-then-check token (issue #1081): the kick's main-thread post applies against
    // whatever context state exists at COMPLETION time — a backend swap or context
    // retirement while the worker ran makes the queued pre-switch cfgCopy poison: its
    // SyncPaneWithBackend would swap the backend BACK (stale-cfg flip-flop) and the seed
    // would push old-backend rows into the new backend's grid.
    const std::uint64_t capturedGeneration = it->second->backendGeneration_.load();
    TrackerConfig cfgCopy = paneCfg;
    LaunchBackgroundTask([this, paneId, cfgCopy, viewId, capturedGeneration]() mutable {
        /* PILLAR2_WORKER_ONLY */ // est-latency: 5ms — smatchet_views.json bucket + cached-ticket seed off the UI
                                  // thread
        ViewsStore views = ConfigManager::LoadViewsOrBootstrap(cfgCopy);
        if (!viewId.empty()) {
            for (size_t i = 0; i < views.Views.size(); ++i) {
                if (views.Views[i].Id == viewId) {
                    views.ActiveViewId = viewId;
                    // Adopt the pane's OWN view query + field set (review HIGH-1): the caller's
                    // cfg snapshot carries the FOCUSED view's JqlQuery/SelectedFields, and all
                    // three fetchers consume cfg.JqlQuery for the actual query (the viewsOverride
                    // re-point only fixes the field list) — without this the unfocused pane's
                    // first sync fetched the focused view's result set.
                    cfgCopy.JqlQuery = views.Views[i].Jql;
                    cfgCopy.SelectedFields = views.Views[i].Fields;
                    break;
                }
            }
        }
        // Durable cross-restart pane snapshot (multi-grid Slice 5a, plan item 21, Deliverable 1):
        // read this pane's LAST-synced rows from tickets_v2 off the UI thread, namespaced by the
        // pane's OWN backend key (backend-key isolation — a GitHub pane reads only GitHub rows).
        // Seeded into ActiveTickets on the main-thread hop below so the grid renders the cached
        // snapshot instantly, before the live fetch completes (a restored pane is no longer a
        // cold blank grid). GetAllTickets uses non-cached local statements, safe under the LCM's
        // OPEN_FULLMUTEX connection.
        std::vector<CachedTicket> seedTickets;
        if (Cache) {
            const std::string seedKey = ConfigManager::NormalizeViewsBackendKey(cfgCopy.TrackerType);
            if (!seedKey.empty()) {
                try {
                    seedTickets = Cache->GetAllTickets(seedKey);
                } catch (const std::exception& ex) {
                    // Non-fatal: an unreadable cache just means no instant snapshot — the live
                    // fetch still populates the grid. Logged, not swallowed (policy).
                    LOG_WARN("AppController::EnsurePaneLiveSyncStarted seed read failed pane='%s' err=%s",
                             paneId.c_str(), ex.what());
                }
            }
        }
        mainThreadDispatcher.PostToMainThread(
            [this, paneId, cfgCopy, views, viewId, capturedGeneration, seedTickets = std::move(seedTickets)]() mutable {
                applyPaneSyncKickOnMainThread_(paneId, std::move(cfgCopy), views, viewId, capturedGeneration,
                                               std::move(seedTickets));
            });
    });
}

void AppController::applyPaneSyncKickOnMainThread_(const std::string& paneId, TrackerConfig cfgCopy,
                                                   const ViewsStore& views, const std::string& viewId,
                                                   std::uint64_t capturedGeneration,
                                                   std::vector<CachedTicket> seedTickets) {
    // UI thread: the context may have been retired while the load ran — find() guards.
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator ctxIt = gridContexts_.find(paneId);
    // Capture-then-check (issue #1081): drop BOTH the sync kick and the cache seed
    // when the context is gone or its backend generation moved since kick — applying
    // the stale cfgCopy would swap the backend BACK (flip-flop) and seed the old
    // backend's rows into the new backend's grid. Re-arm the one-shot latch so the
    // pane re-kicks with FRESH state next frame instead of dead-latching unsynced.
    if (!smatchet::PaneSyncKickStillCurrent(ctxIt != gridContexts_.end(), capturedGeneration,
                                            ctxIt != gridContexts_.end() ? ctxIt->second->backendGeneration_.load()
                                                                         : 0)) {
        LOG_INFO("AppController::EnsurePaneLiveSyncStarted pane='%s' kick discarded — backend "
                 "generation moved (or context retired) mid-kick (issue #1081); latch re-armed.",
                 paneId.c_str());
        if (ctxIt != gridContexts_.end()) {
            ctxIt->second->initialSyncKicked = false;
            // Cleared WITH the latch (GridLiveContext.h discipline): a stale lastSyncedJql
            // would otherwise suppress the JQL-drift re-kick after the fresh kick lands.
            ctxIt->second->lastSyncedJql.clear();
        }
        return;
    }
    if (ctxIt != gridContexts_.end()) {
        // Record the JQL this kick syncs (UI thread only — same discipline as
        // initialSyncKicked). The focus-switch path compares it against the adopted
        // view's saved JQL to detect drift (view edited after the context synced) and
        // re-kick instead of adopting stale rows (review MEDIUM-2). Cleared (with the
        // latch) by the session-end deps hook when the sync fails (review MEDIUM-1).
        ctxIt->second->lastSyncedJql = cfgCopy.JqlQuery;
        // Publish the pane's OWN resolved view (multi-grid Slice 4 cold-start hole):
        // `views` was loaded from the pane's backend bucket, so its matching entry is
        // the pane's real view even when the focused ViewState bucket can't see it. The
        // grid builds this cross-backend pane's columns from it instead of falling back
        // to the focused view's column set on a cold start (no session capture yet).
        if (!viewId.empty()) {
            for (size_t i = 0; i < views.Views.size(); ++i) {
                if (views.Views[i].Id == viewId) {
                    ctxIt->second->resolvedOwnView = std::make_shared<const ViewDefinition>(views.Views[i]);
                    break;
                }
            }
        }
    }
    // Kick the live fetch FIRST: for a fresh context SyncPaneWithBackend →
    // SwapBackendIfTrackerChanged creates the backend (backendSwapped) and CLEARS
    // ActiveTickets. Seeding before this would be wiped — so seed AFTER the swap.
    SyncPaneWithBackend(paneId, &cfgCopy, &views);
    // Seed the just-cleared ActiveTickets with the durable cache rows so the grid shows
    // the last snapshot immediately; the streaming fetch then UPSERTS by id (no double
    // count — DrainPendingStreamingBatches updates-or-pushes per id) and the empty-fetch
    // guard never wipes these rows before a real refresh confirms. Only seed an empty
    // ActiveTickets (a fresh / re-shown husk) so we never clobber rows already applied.
    if (!seedTickets.empty()) {
        std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator seedIt = gridContexts_.find(paneId);
        if (seedIt != gridContexts_.end()) {
            GridLiveContext& ctx = *seedIt->second;
            std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
            if (ctx.ActiveTickets.empty()) {
                ctx.ActiveTickets = std::move(seedTickets);
                ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
                ctx.ActiveTicketsRevision.fetch_add(1);
            }
        }
    }
}

GridLiveContext& AppController::paneContextOrFocused_(const std::string& paneId) {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    return (it == gridContexts_.end()) ? focusedContext() : *it->second;
}

const GridLiveContext& AppController::paneContextOrFocused_(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    return (it == gridContexts_.end()) ? focusedContext() : *it->second;
}

bool AppController::IsPaneSyncLive(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    return it != gridContexts_.end() && it->second->initialSyncKicked;
}

std::string AppController::GetPaneLastSyncedJql(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    return (it == gridContexts_.end()) ? std::string() : it->second->lastSyncedJql;
}

void AppController::RecordPaneSyncKick(const std::string& paneId, const std::string& jql) {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it != gridContexts_.end()) {
        it->second->initialSyncKicked = true;
        it->second->lastSyncedJql = jql;
    }
}

std::shared_ptr<const std::vector<CachedTicket>>
AppController::GetPaneTicketsSnapshot(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(it->second->activeTicketsMutex_);
    return it->second->activeTicketsPublished_;
}

std::uint64_t AppController::GetPaneTicketsRevision(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    return (it == gridContexts_.end()) ? 0 : it->second->ActiveTicketsRevision.load();
}

std::shared_ptr<const ViewDefinition> AppController::GetPaneResolvedView(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    return (it == gridContexts_.end()) ? nullptr : it->second->resolvedOwnView;
}

void AppController::SyncPaneWithBackend(const std::string& paneId, const TrackerConfig* configOverride,
                                        const ViewsStore* viewsOverride) {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it != gridContexts_.end() && it->second->ticketSync_) {
        it->second->ticketSync_->SyncWithBackend(configOverride, viewsOverride);
    }
}

void AppController::TickAllContexts() {
    SMATCHET_UI_PERF_SCOPE("AppController::TickAllContexts");
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    // One frame tick — panes drawn this frame stamp lastVisibleFrame with it; the LRU cap
    // classifies visibility by frame recency (see paneFrameClock_).
    ++paneFrameClock_;
    // Shared drain deadline across N panes' streaming applies (plan item 18): each
    // TicketSyncService::TickStreamingApply is internally budgeted (3 ms / 20 tickets), so N
    // visible panes applying at once could stack N× on one frame. The deadline bounds the
    // total; the ROTATING start order means a deferred context is first in line next frame —
    // the design-addendum § 3.4 verdict (no dispatcher-level round-robin machinery unless the
    // concurrent-sync scenario shows starvation) holds.
    const std::chrono::steady_clock::time_point deadline = start + std::chrono::milliseconds(4);
    const std::size_t n = gridContexts_.size();
    if (n != 0) {
        std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(tickRotation_ % n));
        ++tickRotation_;
        for (std::size_t i = 0; i < n; ++i) {
            if (it == gridContexts_.end()) {
                it = gridContexts_.begin();
            }
            if (it->second->ticketSync_) {
                it->second->ticketSync_->TickStreamingApply();
            }
            ++it;
            if (i + 1 < n && std::chrono::steady_clock::now() >= deadline) {
                break; // remaining contexts run next frame, rotated to the front
            }
        }
    }
    retireExpiredHiddenContexts_(start);
    evictHiddenPanesOverCap_();
}

void AppController::SetWindowFocused(bool focused) {
    // Any-thread (the Standalone host feeds GLFW_FOCUSED from its frame loop). Plain atomic
    // store; TickChangeMonitors reads it once per frame to gate polling on window focus (item 17).
    windowFocused_.store(focused);
}

void AppController::TickChangeMonitors(const TrackerConfig& cfg) {
    SMATCHET_UI_PERF_SCOPE("AppController::TickChangeMonitors");
    // Global gate first — the whole feature is off, so do no per-pane work.
    if (!cfg.TicketChangeMonitorEnabled) {
        return;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::chrono::seconds interval(cfg.TicketChangeMonitorIntervalSec > 0 ? cfg.TicketChangeMonitorIntervalSec
                                                                               : 120);
    const bool backendReachable = GetLastTrackerConnectivityState() == TrackerConnectivityState::AuthenticatedReachable;
    const bool windowFocused = windowFocused_.load();

    for (std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.begin();
         it != gridContexts_.end(); ++it) {
        const std::string& paneId = it->first;
        GridLiveContext& ctx = *it->second;
        // Same recency rule as the LRU cap (default + focused always count; others by frame
        // recency). A hidden/LRU-evicted pane has no reliable in-memory baseline, so it is
        // skipped here — never diffed against an empty set (PaneSyncKickPolicy.h § grill #2).
        const bool paneRecentlyVisible =
            paneId == kDefaultPaneId || paneId == focusedPaneId_ || (paneFrameClock_ - ctx.lastVisibleFrame) <= 1;
        const bool syncActive = ctx.ticketSync_ && ctx.ticketSync_->IsActive();
        if (!smatchet::ShouldPollForChanges(now, /*monitorEnabled=*/true, backendReachable, windowFocused,
                                            paneRecentlyVisible, syncActive, ctx.nextChangePollAt)) {
            continue;
        }
        // First poll per context: establish a SILENT baseline (seed the anchor + next-poll time,
        // no fetch, no toast) so enabling the monitor on an already-populated pane does not
        // replay the whole view as "changes".
        if (!ctx.changeBaselineEstablished) {
            ctx.changeBaselineEstablished = true;
            ctx.changeSinceAnchor = std::chrono::system_clock::now();
            ctx.nextChangePollAt = now + interval;
            continue;
        }
        // Resolve the salient roster from THIS pane's field catalog (UI thread, catalog mutex).
        // No salient fields yet (catalog not loaded) → nothing to diff: push the next poll out
        // and skip WITHOUT disturbing the anchor so the window is retried once the catalog lands.
        std::vector<std::string> salientFieldIds;
        {
            std::lock_guard<std::mutex> lock(ctx.fieldCatalog.availableFieldsMutex_);
            const std::vector<smatchet::SalientFieldRole> roster =
                smatchet::ResolveSalientRoster(ctx.fieldCatalog.AvailableFields);
            salientFieldIds.reserve(roster.size());
            for (std::size_t i = 0; i < roster.size(); ++i) {
                salientFieldIds.push_back(roster[i].fieldId);
            }
        }
        if (salientFieldIds.empty()) {
            ctx.nextChangePollAt = now + interval;
            continue;
        }
        // Latch a strong backend handle ON THE UI THREAD (gridContexts_ / ctx.Backend must not be
        // touched off-thread) — keeps the backend alive across a live swap for the fetch (ADR 0012).
        std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&ctx.Backend);
        if (!backend) {
            ctx.nextChangePollAt = now + interval;
            continue;
        }
        const std::uint64_t capturedGeneration = ctx.backendGeneration_.load();
        const std::chrono::system_clock::time_point sinceAnchor = ctx.changeSinceAnchor;
        // polledAt = the instant we ISSUE this poll; the next poll's anchor advances to it so a
        // change landing DURING the fetch is caught next round (no gap).
        const std::chrono::system_clock::time_point polledAt = std::chrono::system_clock::now();
        // Window for the backend-native "updated >=" filter: time since the anchor plus a margin
        // to absorb minute-granularity timestamps and clock skew.
        const std::chrono::seconds window =
            std::chrono::duration_cast<std::chrono::seconds>(polledAt - sinceAnchor) + std::chrono::seconds(60);
        // In-flight guard: push nextChangePollAt out NOW so the per-frame tick does not
        // re-dispatch while this probe runs.
        ctx.nextChangePollAt = now + interval;

        TrackerConfig cfgCopy = cfg;
        const std::string paneJql = ctx.lastSyncedJql; // pane's own query (empty → cfg's focused query)
        // Snapshot the pane's CURRENT cached keys ON THE UI THREAD — the membership reconcile's
        // baseline (item 10). The worker diffs these against the view's full key list to find rows
        // that vanished; ctx must not be touched off-thread, so it is captured by value.
        std::vector<std::string> cachedKeys;
        {
            std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
            cachedKeys.reserve(ctx.ActiveTickets.size());
            for (std::size_t i = 0; i < ctx.ActiveTickets.size(); ++i) {
                cachedKeys.push_back(ctx.ActiveTickets[i].id);
            }
        }
        LaunchBackgroundTask([this, paneId, backend, cfgCopy, paneJql, salientFieldIds, cachedKeys, window, polledAt,
                              capturedGeneration]() mutable {
            /* PILLAR2_WORKER_ONLY */ // est-latency: one view fetch — off the UI thread
            ViewsStore views = ConfigManager::LoadViewsOrBootstrap(cfgCopy);
            if (!paneJql.empty()) {
                cfgCopy.JqlQuery = paneJql;
            }
            auto res = backend->Reader().FetchIssuesChangedSince(cfgCopy, views, window, salientFieldIds);
            if (!static_cast<bool>(res)) {
                // Soft failure (partial/warned fetch, transport): leave the anchor untouched so
                // the next interval retries the same window. Logged, never toasted.
                LOG_INFO("AppController::TickChangeMonitors probe skipped pane='%s': %s", paneId.c_str(),
                         res.error().Detail.c_str());
                return;
            }
            std::vector<CachedTicket> fetched = std::move(res.value());
            // Membership reconcile (item 10): which cached rows does the view no longer list?
            // Off-thread, static — touches no instance state. See computeMembershipRemovals_.
            std::vector<smatchet::MembershipRemovalVerdict> verdicts =
                computeMembershipRemovals_(backend->Reader(), cfgCopy, views, cachedKeys, paneId);
            mainThreadDispatcher.PostToMainThread([this, paneId, fetched = std::move(fetched),
                                                   verdicts = std::move(verdicts), polledAt,
                                                   capturedGeneration]() mutable {
                applyChangeProbeOnMainThread_(paneId, std::move(fetched), std::move(verdicts), polledAt,
                                              capturedGeneration);
            });
        });
    }
}

std::vector<smatchet::MembershipRemovalVerdict>
AppController::computeMembershipRemovals_(ITrackerIssueReader& reader, const TrackerConfig& cfg,
                                          const ViewsStore& views, const std::vector<std::string>& cachedKeys,
                                          const std::string& paneId) {
    // FetchIssueKeysForView is a light key-only projection; per vanished key, ProbeIssueExists
    // separates "left the view" (200, the conservative default → keep the shared cache row, toast
    // LeftView) from "deleted" (404 → drop the row, toast Deleted). A keys-fetch soft failure skips
    // the reconcile this cycle (the changed-since detection toast still fires); a probe error is
    // non-destructive. Static — touches no AppController instance state, safe on the worker thread.
    std::vector<smatchet::MembershipRemovalVerdict> verdicts;
    auto keysRes = reader.FetchIssueKeysForView(cfg, views);
    if (!static_cast<bool>(keysRes)) {
        LOG_INFO("AppController::TickChangeMonitors keys-fetch skipped pane='%s': %s", paneId.c_str(),
                 keysRes.error().Detail.c_str());
        return verdicts;
    }
    const std::vector<std::string> removed = smatchet::RemovedKeys(cachedKeys, keysRes.value());
    const std::size_t kMaxProbesPerCycle = 25; // lightweight cap; remainder retried next cycle
    for (std::size_t i = 0; i < removed.size() && i < kMaxProbesPerCycle; ++i) {
        auto probe = reader.ProbeIssueExists(cfg, removed[i]);
        smatchet::MembershipRemovalVerdict v;
        v.issueKey = removed[i];
        v.stillExists = static_cast<bool>(probe) ? probe.value() : true; // probe error → non-destructive
        verdicts.push_back(v);
    }
    if (removed.size() > kMaxProbesPerCycle) {
        LOG_INFO("AppController::TickChangeMonitors reconcile capped pane='%s': %zu vanished, "
                 "probing %zu this cycle",
                 paneId.c_str(), removed.size(), kMaxProbesPerCycle);
    }
    return verdicts;
}

void AppController::applyChangeProbeOnMainThread_(const std::string& paneId, std::vector<CachedTicket> fetched,
                                                  std::vector<smatchet::MembershipRemovalVerdict> verdicts,
                                                  std::chrono::system_clock::time_point polledAt,
                                                  std::uint64_t capturedGeneration) {
    // UI thread. Re-find the context (retired mid-flight → drop) and re-check the backend
    // generation (swapped mid-flight → drop): a stale apply would diff new-backend rows against
    // the old baseline (capture-then-check, issue #1081).
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator ctxIt = gridContexts_.find(paneId);
    if (!smatchet::PaneSyncKickStillCurrent(ctxIt != gridContexts_.end(), capturedGeneration,
                                            ctxIt != gridContexts_.end() ? ctxIt->second->backendGeneration_.load()
                                                                         : 0)) {
        return;
    }
    GridLiveContext& ctx = *ctxIt->second;
    // Re-resolve the roster on the UI thread (the catalog rarely moves within one fetch RTT) and
    // snapshot the pane's CURRENT in-memory cache as the diff baseline (prev). Both under their
    // own mutex; the diff itself is pure.
    std::vector<smatchet::SalientFieldRole> roster;
    {
        std::lock_guard<std::mutex> lock(ctx.fieldCatalog.availableFieldsMutex_);
        roster = smatchet::ResolveSalientRoster(ctx.fieldCatalog.AvailableFields);
    }
    std::vector<CachedTicket> prev;
    {
        std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
        prev = ctx.ActiveTickets;
    }
    std::vector<smatchet::TicketChangeSummary> changes = smatchet::DiffChangedTickets(prev, fetched, roster);

    // Fold in the membership reconcile (item 10). Classify the probe verdicts against the pane's
    // roster at apply time (residentIds = `prev`'s ids; UI thread, no mutation since the snapshot):
    // resident keys are erased + toasted (LeftView/Deleted); every 404 verdict purges the shared
    // per-backend cache row regardless of pane residency (the cache spans panes). A verdict for a
    // key the pane no longer holds is a silent in-memory no-op. See ClassifyMembershipRemovals.
    std::vector<std::string> residentIds;
    residentIds.reserve(prev.size());
    for (std::size_t i = 0; i < prev.size(); ++i) {
        residentIds.push_back(prev[i].id);
    }
    const smatchet::MembershipReconcilePlan plan = smatchet::ClassifyMembershipRemovals(residentIds, verdicts);
    const std::string backendKey = ctx.CacheBackendKeyCopy();
    if (Cache) {
        for (std::size_t i = 0; i < plan.cacheDeleteKeys.size(); ++i) {
            Cache->DeleteTicket(backendKey, plan.cacheDeleteKeys[i]); // 404 → drop the shared cache row
        }
    }
    if (!plan.residentRemovals.empty()) {
        {
            // Erase every resident removal + republish the immutable snapshot + bump the revision
            // once under one lock (same contract as TicketSyncService::DrainStaleDeletionBudget) so
            // readers see the shrunk roster atomically.
            std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
            for (std::size_t i = 0; i < plan.residentRemovals.size(); ++i) {
                const std::string& key = plan.residentRemovals[i].issueKey;
                ctx.ActiveTickets.erase(std::remove_if(ctx.ActiveTickets.begin(), ctx.ActiveTickets.end(),
                                                       [&key](const CachedTicket& t) { return t.id == key; }),
                                        ctx.ActiveTickets.end());
            }
            ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
            ctx.ActiveTicketsRevision.fetch_add(1);
        }
        for (std::size_t i = 0; i < plan.residentRemovals.size(); ++i) {
            const smatchet::MembershipRemovalVerdict& v = plan.residentRemovals[i];
            smatchet::TicketChangeSummary s;
            s.kind = v.stillExists ? smatchet::TicketChangeKind::LeftView : smatchet::TicketChangeKind::Deleted;
            s.issueId = v.issueKey;
            changes.push_back(s);
        }
        // Mark Lua cells for re-record (the visible roster shrank) — same one-shot signal the
        // stale-delete prune uses; no-op in the stub build. UI thread, cheap.
        NotifyLuaTicketDataChanged();
    }
    if (!changes.empty()) {
        NotifyTicketChanges(changes);
    }
    // Advance the anchor to the poll-issue instant (no gap). nextChangePollAt was already pushed
    // out at dispatch, so this slice does not re-stamp it here.
    ctx.changeSinceAnchor = polledAt;
}

void AppController::evictHiddenPanesOverCap_() {
    // Hidden-pane LRU memory cap (multi-grid Slice 5a, plan item 21, Deliverable 2). Builds the
    // pure-decision candidate snapshot, then drops the least-recently-visible idle hidden pane's
    // in-memory ActiveTickets while over cap. Rows survive in tickets_v2, so re-showing the pane
    // re-seeds losslessly (initialSyncKicked is re-armed so EnsurePaneLiveSyncStarted re-runs).
    std::vector<smatchet::HiddenPaneEvictionCandidate> candidates;
    candidates.reserve(gridContexts_.size());
    for (std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.begin();
         it != gridContexts_.end(); ++it) {
        GridLiveContext& ctx = *it->second;
        smatchet::HiddenPaneEvictionCandidate c;
        c.paneId = it->first;
        // The default pane (permanent, holds the focused snapshot + offline-queue chain) and the
        // focused pane are never hidden for cap purposes. A drawn pane stamps lastVisibleFrame
        // with the current paneFrameClock_, so treating it as visible when drawn this frame or
        // the previous one is FPS-independent (the old wall-clock window evicted a
        // genuinely-visible pane at low FPS, flickering it — review finding).
        c.isVisible =
            it->first == kDefaultPaneId || it->first == focusedPaneId_ || (paneFrameClock_ - ctx.lastVisibleFrame) <= 1;
        c.hasActiveSync = ctx.ticketSync_ && ctx.ticketSync_->IsActive();
        {
            std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
            c.hasResidentRows = !ctx.ActiveTickets.empty();
        }
        c.lastVisibleOrder = ctx.lastVisibleOrder;
        candidates.push_back(std::move(c));
    }

    for (;;) {
        const std::string victimId = smatchet::ChooseHiddenPaneToEvict(candidates, hiddenPaneResidentCap_);
        if (victimId.empty()) {
            break;
        }
        std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(victimId);
        if (it != gridContexts_.end()) {
            GridLiveContext& ctx = *it->second;
            {
                std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
                std::vector<CachedTicket>().swap(ctx.ActiveTickets);
                ctx.activeTicketsPublished_.reset();
                // Bump the revision inside the lock — matches the seed + stale-drain paths so a
                // reader never sees a null published pointer paired with the pre-bump revision.
                ctx.ActiveTicketsRevision.fetch_add(1);
            }
            // Re-arm the one-shot first-sync latch so the next time this pane is shown,
            // EnsurePaneLiveSyncStarted re-seeds from tickets_v2 + re-syncs (the cap dropped the
            // in-memory snapshot, not the durable rows). Idle-only (busy panes are excluded by
            // the policy), so no worker is racing this flag.
            ctx.initialSyncKicked = false;
            LOG_INFO("AppController: LRU-evicted hidden pane '%s' in-memory tickets (cap=%zu)", victimId.c_str(),
                     hiddenPaneResidentCap_);
        }
        // Reflect the eviction in the local snapshot so the next iteration recomputes the count.
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].paneId == victimId) {
                candidates[i].hasResidentRows = false;
                break;
            }
        }
    }
}

void AppController::retireExpiredHiddenContexts_(std::chrono::steady_clock::time_point now) {
    // Issue #1457: hold the map mutex across the whole iterate + erase so the User Info worker's
    // snapshot-find never traverses a tree this loop is restructuring. Lock ordering is map-mutex
    // OUTERMOST then the per-context activeTicketsMutex_ / availableFieldsMutex_ taken below —
    // consistent with the worker (map then, separately, a per-context mutex), so no inversion. The
    // ticketSync_.reset() join here is idle-only (busy syncs postpone via the IsActive() continue),
    // so the joined worker has already exited and cannot re-take this mutex during the join.
    std::lock_guard<std::mutex> mapLk(gridContextsMutex_);
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.begin();
    while (it != gridContexts_.end()) {
        GridLiveContext& ctx = *it->second;
        const bool retirable = it->first != kDefaultPaneId && it->first != focusedPaneId_ && ctx.everVisible &&
                               (now - ctx.lastVisibleAt) >= kHiddenContextGrace;
        if (!retirable || (ctx.ticketSync_ && ctx.ticketSync_->IsActive())) {
            // Busy sync: postpone — the join inside ~TicketSyncService must stay instant
            // (Pillar 2: no UI-thread block). Retried next frame.
            ++it;
            continue;
        }
        LOG_INFO("AppController: retiring hidden GridLiveContext for pane '%s'", it->first.c_str());
        ctx.ticketSync_.reset(); // idle — worker join returns immediately
        // Invalidate in-flight work captured against this context (issue #1081): the husk
        // outlives retirement (graveyard below), so a worker holding a latched pointer must
        // see the generation move and drop its apply (capture-then-check).
        ctx.backendGeneration_.fetch_add(1);
        std::shared_ptr<ITrackerBackend> oldBackend =
            std::atomic_exchange(&ctx.Backend, std::shared_ptr<ITrackerBackend>());
        if (oldBackend) {
            RetireBackend(std::move(oldBackend)); // ADR-0012 graveyard (raw subobject ptrs stay valid)
        }
        {
            std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
            std::vector<CachedTicket>().swap(ctx.ActiveTickets);
            ctx.activeTicketsPublished_.reset();
        }
        {
            // Husk hygiene: the retired context stays alive for latched readers (graveyard
            // below), but must not retain a fully-populated field catalog — that would grow
            // unboundedly over show/hide cycles. Clear under the catalog mutex so a worker
            // that latched this context's catalog sees empty (not freed) containers.
            std::lock_guard<std::mutex> lock(ctx.fieldCatalog.availableFieldsMutex_);
            std::vector<TrackerField>().swap(ctx.fieldCatalog.AvailableFields);
            std::vector<TrackerComponent>().swap(ctx.fieldCatalog.AvailableComponents);
            std::vector<TrackerIssueTypeCreateMeta>().swap(ctx.fieldCatalog.AvailableIssueTypeMeta);
            std::vector<TrackerUser>().swap(ctx.fieldCatalog.AvailableUsers);
            ctx.fieldCatalog.projectComponentOptions_.clear();
        }
        // Defer-free husk: a worker may have latched a pointer to this context (it was the
        // focused context once) — keep the now-tiny object alive until ~AppController, the
        // ADR-0012 graveyard pattern applied to contexts.
        retiredContexts_.push_back(std::move(it->second));
        paneAdapters_.erase(it->first);
        it = gridContexts_.erase(it);
    }
}

AppController::~AppController() {

    // Shutdown ordering matters here — every background thread that can post to mainThreadDispatcher
    // or read `this` via __smatchet_app must be joined BEFORE member destruction begins. This
    // matches the contract described in MainThreadDispatcher.h and AppController_LuaBindings.cpp.

    // Phase 4b of docs/plans/shipped/smatchet-merge-watcher.md — stop the merge-watch
    // HTTP server FIRST. Its listen thread queues toast-append lambdas via
    // mainThreadDispatcher; the join here guarantees no late post races
    // BeginShutdown() below.
    if (mergeWatchNotifyServer_) {
        mergeWatchNotifyServer_->Stop();
        mergeWatchNotifyServer_.reset();
    }
#if defined(SMATCHET_WITH_AI)
    // Drop the AI assistant first — its worker may still be inside SendStreaming. The
    // dtor flips the cancel atom + joins; only after the join can the main-thread
    // dispatcher safely BeginShutdown(), because callbacks already in flight will
    // hand off through `mainThreadDispatcher.PostToMainThread` (still accepting posts
    // at this instant) and the controller's join blocks until those callbacks return.
    impl_->aiAssistant_.reset();
    // Phase 3 of ai-chat-claude-desktop-parity. Stop the chat-persist worker after
    // the AI assistant finishes (no more new Enqueue calls from a streaming callback)
    // but BEFORE `mainThreadDispatcher.BeginShutdown()` and before LCM destructs at
    // member-destruction time. Stop drains pending ops within 250 ms then joins the
    // worker thread. Pillar 3 — no detached thread may outlive `Cache`.
    smatchet::ai::chat_persist::Stop();
#endif

    // Stop the coalescing config-save worker: flush pending config writes within a bounded budget,
    // then join. Not AI-gated — config saves happen regardless of feature flags. After this, any
    // further config save falls back to a synchronous write on the caller.
    smatchet::config_save::Stop();

    mainThreadDispatcher.BeginShutdown();

    CancelAndJoinActiveStreamingSync();

#if defined(SMATCHET_WITH_LUA_AUTOMATION)

    ClearLuaTicketContextGlue();

    {

        std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);

        impl_->automationWorkerShuttingDown_.store(true);
    }

    // Abort any in-flight blocking tracker mutation the worker is parked inside (UpdateIssueFields →
    // TrackerPut/Patch/PostLogged). The retry/backoff loop polls this token before each attempt and at
    // 50 ms granularity during backoff (#1529), so a job blocked in synchronous tracker glue — which
    // executes no Lua and so never fires the count-hook — stops retrying immediately and unwinds. A
    // single already-issued HTTP request still runs to its own kTrackerOverallTimeoutMs, but the
    // pathological case (up to maxAttempts × timeout of serial retries) is gone, so the unconditional
    // join() below completes within one request timeout at most — not the multi-retry worst case — and
    // still without .detach (the worker returns from the cancelled call and breaks its loop).
    automationShutdownCancel_->store(true);

    impl_->automationJobCv_.notify_all();

    if (impl_->automationWorker_.joinable()) {
        // Bounded, observable shutdown wait. The count-hook releases a pure-Lua job
        // the instant automationWorkerShuttingDown_ is seen, but a job blocked inside
        // synchronous C++ glue (a tracker HTTP PUT — see LuaAutomationHookPolicyPure.h)
        // executes no Lua instructions, so the hook never fires and an unbounded join
        // would hang here until the HTTP timeout with no diagnostic. Wait on the
        // worker-exited flag up to a deadline; if it overruns, log a loud WARN naming
        // the hang, then still join — abandoning the thread (.detach) is banned and
        // unsafe (it could touch freed Impl state), so the warned bounded wait is the
        // safe terminal: the join still completes once the blocking call returns.
        constexpr std::chrono::seconds kAutomationJoinWarnDeadline(5);
        {
            std::unique_lock<std::mutex> lock(impl_->automationJobMutex_);
            const bool exited = impl_->automationJobCv_.wait_for(
                lock, kAutomationJoinWarnDeadline, [this]() { return impl_->automationWorkerExited_.load(); });
            if (!exited) {
                LOG_WARN("AppController shutdown: automation worker did not exit within %llds — likely blocked in "
                         "synchronous tracker glue (e.g. an in-flight HTTP request). Waiting for it to return; "
                         "shutdown is bounded by that call's own timeout, not hung.",
                         static_cast<long long>(kAutomationJoinWarnDeadline.count()));
            }
        }
        impl_->automationWorker_.join();
    }

#endif

    // Join UiDrawSession std::async futures that captured this controller (via static `g_ui`) before

    // tearing down members other threads may still touch.

    DrainUiDrawSessionFuturesBeforeAppTeardown(*this);

    shuttingDown_.store(true);

    // Drain the in-flight connectivity probe future before joining the background pool — the FSM's
    // std::async worker must not outlive the service (Phase 3 R7). Ordering preserved from the prior
    // DrainTrackerConnectivityProbeFuture() site.
    if (connectivity_) {
        connectivity_->DrainProbeFuture();
    }

    JoinBackgroundTasks();

    // A4 (BACKLOG_CODE_REVIEW.md): every background thread that can emit a log line is joined
    // above, so flush the async file sink now — this guarantees the whole shutdown-sequence log
    // trail reaches disk before member destruction and the riskier late-teardown steps
    // (mainWindow/pluginHost reset, static-destructor time) run. The abrupt-CRASH path is
    // deliberately NOT flushed through the logger: SmatchetCrashHandler is async-signal-safe and
    // must not take the file-sink mutex / wait on its ack condvar mid-crash (deadlock risk) — a
    // crash captures its trail via that handler's separate async-safe crash sink instead.
    Logger::Instance().FlushFileSink();
}

std::shared_ptr<const std::vector<CachedTicket>> AppController::GetActiveTicketsSnapshot() const {

    const GridLiveContext& ctx = focusedContext();

    std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);

    if (!ctx.activeTicketsPublished_) {

        ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
    }

    return ctx.activeTicketsPublished_;
}

std::vector<CachedTicket> AppController::GetActiveTickets() const {

    const GridLiveContext& ctx = focusedContext();

    std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);

    if (!ctx.activeTicketsPublished_) {

        ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
    }

    return *ctx.activeTicketsPublished_;
}

void AppController::PrefetchIssueTicketsForKeys(const std::vector<std::string>& issueKeys, bool includeAlreadyActive) {

    if (!Cache) {

        return;
    }

    std::vector<std::string> toFetch;

    {

        const auto snap = GetActiveTicketsSnapshot();

        std::unordered_set<std::string> have;

        if (snap) {

            for (const auto& t : *snap) {

                have.insert(t.id);
            }
        }

        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);

        for (const auto& k : issueKeys) {

            if (k.empty() || (!includeAlreadyActive && have.count(k) > 0)) {

                continue;
            }

            if (bulkImportPrefetchKeysInFlight_.count(k) > 0) {

                continue;
            }

            bulkImportPrefetchKeysInFlight_.insert(k);

            toFetch.push_back(k);
        }
    }

    if (toFetch.empty()) {

        return;
    }

    LaunchBackgroundTask([this, toFetch]() { FetchAndCachePrefetchedTickets(toFetch); });
}

void AppController::FetchAndCachePrefetchedTickets(const std::vector<std::string>& toFetch) {
    // Latch a strong handle via atomic_load: this worker reads Backend off the UI thread,
    // which would race a live SetBackend swap on a plain .get(). The shared_ptr also keeps
    // the backend alive for the FetchIssuesForKeys call (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);

    if (!backend) {

        return;
    }

    TrackerConfig cfg = ConfigManager::Load();

    ViewsStore views = ConfigManager::LoadViewsOrBootstrap(cfg);

    std::string err;

    std::vector<CachedTicket> tickets;

    auto fetchResult = backend->Reader().FetchIssuesForKeys(cfg, toFetch, views);
    const bool ok = static_cast<bool>(fetchResult);
    if (ok) {
        tickets = std::move(fetchResult.value());
    } else {
        err = fetchResult.error().Detail;
    }

    {

        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);

        for (const auto& k : toFetch) {

            bulkImportPrefetchKeysInFlight_.erase(k);
        }
    }

    if (!ok) {

        if (IsTrackerTransportErrorText(err)) {

            LOG_INFO("AppController::PrefetchIssueTicketsForKeys skipped (transport): %s", err.c_str());

        } else {

            LOG_WARN("AppController::PrefetchIssueTicketsForKeys failed: %s", err.c_str());
        }

        return;
    }

    requestDeferredLiveTrackerBackendSuccessNotify_();

    if (!Cache) {

        return;
    }

    // Mutex-guarded copy — this runs on a background worker while the UI thread may swap
    // the tracker (and re-stamp the key) concurrently (multi-grid Slice 1b).
    const std::string cacheBackendKey = focusedContext().CacheBackendKeyCopy();

    for (const auto& t : tickets) {

        Cache->SaveTicket(cacheBackendKey, t);
    }

    RefreshLocalData();
}

bool AppController::IsBulkImportPrefetchInFlight(const std::string& issueKey) const {

    if (issueKey.empty()) {

        return false;
    }

    std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);

    return bulkImportPrefetchKeysInFlight_.find(issueKey) != bulkImportPrefetchKeysInFlight_.end();
}

void AppController::LaunchBackgroundTask(std::function<void()> task) {

    if (!task || shuttingDown_.load()) {

        return;
    }

    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([this, task = std::move(task), done]() mutable {
        if (!shuttingDown_.load()) {
            // An exception escaping a worker thread function calls std::terminate
            // (the UI-thread SEH guard does NOT cover worker threads) — a single
            // throw in any background task (e.g. a backend-switch race in the
            // sync worker, a JSON-parse failure on an unexpected API body) takes
            // the whole app down. Contain it: log + abandon the one task, app
            // stays up (Pillar-3 never-crash / graceful degradation). Firewall
            // algorithm is the pure BackgroundTaskFirewall.h (unit-tested).
            std::string what;
            const smatchet::BackgroundTaskOutcome outcome = smatchet::RunBackgroundTaskFirewalled(task, what);
            if (outcome == smatchet::BackgroundTaskOutcome::StdException) {
                LOG_ERROR("AppController: background task threw std::exception: %s — task abandoned, app continues.",
                          what.c_str());
            } else if (outcome == smatchet::BackgroundTaskOutcome::UnknownException) {
                LOG_ERROR("AppController: background task threw a non-std exception — task abandoned, app continues.");
            }
        }
        // Mark complete LAST so the pool's reap (which joins only done workers) sees a thread
        // that's past task() — join() then returns essentially immediately.
        done->store(true, std::memory_order_release);
    });

    std::lock_guard<std::mutex> lock(backgroundWorkersMutex_);
    // Amortized mid-session reap: join + drop any previously-finished workers so the vector
    // tracks only in-flight (plus just-finished) work, not every task ever spawned.
    reapFinishedBackgroundWorkersLocked_();
    backgroundWorkers_.push_back(BackgroundWorker{std::move(worker), std::move(done)});
}

void AppController::reapFinishedBackgroundWorkersLocked_() {
    const std::thread::id selfId = std::this_thread::get_id();
    smatchet::ReapFinishedWorkers(
        backgroundWorkers_,
        [&](const BackgroundWorker& w) {
            // Reapable = task finished (done set) AND not the calling worker itself (never
            // self-join; leave that one for JoinBackgroundTasks at shutdown).
            const bool finished = w.done && w.done->load(std::memory_order_acquire);
            const bool isSelf = w.thread.joinable() && w.thread.get_id() == selfId;
            return finished && !isSelf;
        },
        [](BackgroundWorker& w) {
            if (w.thread.joinable()) {
                w.thread.join(); // done flag set → returns immediately
            }
        });
}

void AppController::RetireBackend(std::shared_ptr<ITrackerBackend> old) {
    // Defer-free (ADR 0012): keep a swapped-out backend alive until shutdown so raw subobject
    // pointers (Reader/Mutations/Connectivity) captured by in-flight workers before the live
    // tracker swap can't dangle. Drained by ~retiredBackends_ in ~AppController, after every
    // worker has been joined via JoinBackgroundTasks.
    if (!old) {
        return;
    }
    std::lock_guard<std::mutex> lk(retiredBackendsMutex_);
    retiredBackends_.push_back(std::move(old));
}

void AppController::JoinBackgroundTasks() {

    std::vector<BackgroundWorker> workers;

    {

        std::lock_guard<std::mutex> lock(backgroundWorkersMutex_);

        workers = std::move(backgroundWorkers_);
    }

    const std::thread::id selfId = std::this_thread::get_id();

    for (auto& worker : workers) {

        if (!worker.thread.joinable()) {

            continue;
        }

        if (worker.thread.get_id() == selfId) {
            // A background worker called JoinBackgroundTasks — cannot self-join.
            // Re-queue so ~AppController can join it from the main thread.
            // Detaching was the previous behaviour but let detached threads outlive
            // the controller and race against g_TrackerIssueFetchMutex teardown.
            LOG_WARN("AppController::JoinBackgroundTasks: self-join detected; re-queuing thread for main-thread join.");
            std::lock_guard<std::mutex> requeue_lock(backgroundWorkersMutex_);
            backgroundWorkers_.push_back(std::move(worker));
            continue;
        }

        worker.thread.join();
    }
}

void AppController::SetOpenUrlHandler(std::function<void(const std::string&)> handler) {

    hostCallbacks_.OpenUrl = std::move(handler);
}

void AppController::SetCloseEmbeddedUiHandler(std::function<void()> handler) {

    hostCallbacks_.CloseEmbeddedUi = std::move(handler);
}

void AppController::CloseEmbeddedUi() {

    if (hostCallbacks_.CloseEmbeddedUi) {

        hostCallbacks_.CloseEmbeddedUi();
    }
}

void AppController::SetRequestAppQuitHandler(std::function<void()> handler) {
    hostCallbacks_.RequestAppQuit = std::move(handler);
}

void AppController::RequestAppQuit() const {

    if (hostCallbacks_.RequestAppQuit) {

        hostCallbacks_.RequestAppQuit();
    }
}

void AppController::SetRuntimePluginHost(PluginHost* host) { runtimePluginHost_ = host; }

#if defined(SMATCHET_WITH_MCP)

namespace {

std::string PrefixMcpActivityLine(const std::string& msg) {

    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tmBuf{};

#if defined(_WIN32)

    if (localtime_s(&tmBuf, &t) != 0) {

        return msg;
    }

#else

    if (localtime_r(&t, &tmBuf) == nullptr) {

        return msg;
    }

#endif

    char timeBuf[32];

    if (std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf) == 0) {

        return msg;
    }

    return std::string(timeBuf) + " " + msg;
}

} // namespace

void AppController::AppendMcpActivity(const std::string& line) {

    std::lock_guard<std::mutex> lock(impl_->mcpActivityMutex_);

    impl_->mcpActivityLog_.push_back(PrefixMcpActivityLine(line));

    while (impl_->mcpActivityLog_.size() > impl_->kMcpActivityLogMax) {

        impl_->mcpActivityLog_.pop_front();
    }
}

std::vector<std::string> AppController::CopyMcpActivityLog() const {

    std::lock_guard<std::mutex> lock(impl_->mcpActivityMutex_);

    return std::vector<std::string>(impl_->mcpActivityLog_.begin(), impl_->mcpActivityLog_.end());
}

void AppController::NotifyMcpClientHttpActivity() {

    mcpHttpTrafficEpoch_.fetch_add(1, std::memory_order_relaxed);

    const auto now = std::chrono::steady_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    mcpLastClientHttpActivityNs_.store(static_cast<std::uint64_t>(ns), std::memory_order_release);
}

std::uint64_t AppController::GetMcpHttpTrafficEpoch() const {

    return mcpHttpTrafficEpoch_.load(std::memory_order_acquire);
}

bool AppController::TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const {

    const std::uint64_t raw = mcpLastClientHttpActivityNs_.load(std::memory_order_acquire);

    if (raw == 0 || out == nullptr) {

        return false;
    }

    *out = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(raw)));

    return true;
}

#endif

void AppController::OpenUrl(const std::string& url) const {

    if (url.empty()) {

        return;
    }

    // Scheme allowlist: avoid handing `javascript:`, `file:`, `vbscript:`, etc.

    // Only http(s) and mailto pass through; anything else is rejected.

    {

        std::string schemePrefix;

        const size_t colonPos = url.find(':');

        if (colonPos == std::string::npos) {

            LOG_WARN("AppController::OpenUrl rejected: missing scheme in url=%s", TruncateForLog(url, 200).c_str());

            return;
        }

        schemePrefix.reserve(colonPos);

        for (size_t i = 0; i < colonPos; ++i) {

            schemePrefix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(url[i]))));
        }

        const bool ok = (schemePrefix == "http") || (schemePrefix == "https") || (schemePrefix == "mailto");

        if (!ok) {

            LOG_WARN("AppController::OpenUrl rejected: scheme '%s' not allowlisted for url=%s", schemePrefix.c_str(),

                     TruncateForLog(url, 200).c_str());

            return;
        }
    }

    if (hostCallbacks_.OpenUrl) {

        hostCallbacks_.OpenUrl(url);

        return;
    }

#if defined(_WIN32)

    const HINSTANCE openResult = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    if (reinterpret_cast<intptr_t>(openResult) <= 32) {

        LOG_ERROR("AppController::OpenUrl failed url=%s err=%ld", TruncateForLog(url, 300).c_str(), GetLastError());
    }

#elif defined(__APPLE__)

    if (!LaunchCommandNoShell("open", url)) {

        LOG_ERROR("AppController::OpenUrl failed to launch url=%s", TruncateForLog(url, 300).c_str());
    }

#else

    if (!LaunchCommandNoShell("xdg-open", url)) {

        LOG_ERROR("AppController::OpenUrl failed to launch url=%s", TruncateForLog(url, 300).c_str());
    }

#endif
}

// AddAutomationLogSink / ClearAutomationLogSinks moved to LuaAutomationHost in Phase 1A of
// the item 14 extraction. Thin delegators below.

void AppController::AddAutomationLogSink(std::function<void(const std::string&)> sink) {
    if (impl_->luaHost_) {
        impl_->luaHost_->AddAutomationLogSink(std::move(sink));
    } else {
        // OnEarlyInit fires before Initialize constructs luaHost_. Buffer the sink;
        // Initialize drains pendingLogSinks_ into luaHost_ immediately after construction.
        if (sink) {
            pendingLogSinks_.push_back(std::move(sink));
        }
    }
}

void AppController::ClearAutomationLogSinks() {
    if (impl_->luaHost_) {
        impl_->luaHost_->ClearAutomationLogSinks();
    }
}

void AppController::AddAutomationErrorSink(std::function<void(const std::string&)> sink) {
    errorSinks_.push_back(std::move(sink));
}

bool AppController::ConsumeScriptingWindowRequest() { return scriptingWindowOpenRequested_.exchange(false); }

void AppController::SetAttachmentViewerHandler(AttachmentViewerHandler handler) {

    hostCallbacks_.AttachmentViewer = std::move(handler);
}

void AppController::SetAttachmentPreviewHandler(AttachmentPreviewHandler handler) {

    hostCallbacks_.AttachmentPreview = std::move(handler);
}

void AppController::SetAttachmentCollectionHandler(AttachmentCollectionHandler handler) {

    hostCallbacks_.AttachmentCollection = std::move(handler);
}

void AppController::SetOpenFilePathsHandler(OpenFilePathsHandler handler) {

    hostCallbacks_.OpenFilePaths = std::move(handler);
}

void AppController::RequestOpenFilePaths(bool allowMultiple, const std::string& initialDirectoryUtf8,

                                         std::function<void(std::vector<std::string>)> onComplete) const {

    if (!onComplete) {

        return;
    }

    if (hostCallbacks_.OpenFilePaths) {

        hostCallbacks_.OpenFilePaths(allowMultiple, initialDirectoryUtf8, std::move(onComplete));

        return;
    }

    onComplete({});
}

// App-update surface — thin delegators forwarding into AttachmentAppUpdateService (god-object
// decomposition Phase 4). The full implementations (semantic-version parse/compare, the GitHub
// releases query, the installer download + launch) moved verbatim into AttachmentAppUpdateService.cpp
// along with their file-local helpers; the public signatures are preserved so the UI / command /
// diagnostics call sites compile unchanged.
std::string AppController::GetAppVersion() const { return attachmentAppUpdate_->GetAppVersion(); }

std::string AppController::GetGitHubReleaseRepo() const { return attachmentAppUpdate_->GetGitHubReleaseRepo(); }

AppUpdateInfo AppController::CheckForAppUpdate(bool includePrerelease) const {
    return attachmentAppUpdate_->CheckForAppUpdate(includePrerelease);
}

bool AppController::DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                                     std::string& outError,
                                                     std::shared_ptr<std::atomic<bool>> cancelFlag) const {
    return attachmentAppUpdate_->DownloadAndLaunchInstallerUpdate(downloadUrl, assetName, outError, cancelFlag);
}

bool AppController::IsOnUiThread() const {
    // uiThreadId_ is written once in Initialize before any worker is spawned, then never
    // mutated. Reads from worker threads are race-free under publish-once semantics.
    return std::this_thread::get_id() == uiThreadId_;
}

void AppController::PostToMainThread(std::function<void()> fn) {
    // IMainThreadPoster — delegate to the concrete dispatcher. Lets the Commands/
    // helper templates marshal onto the UI thread through the interface without
    // including AppController.h (core-include-dag Phase 2).
    mainThreadDispatcher.PostToMainThread(std::move(fn));
}

#if defined(SMATCHET_WITH_AI)
bool AppController::HasAiAssistantController() const { return impl_->aiAssistant_ != nullptr; }

AiAssistantController& AppController::GetAiAssistantController() {
    // No lazy construction. Callers MUST guard with HasAiAssistantController()
    // first — the controller is constructed exactly once at the end of
    // AppController::Initialize and destroyed at the top of ~AppController.
    // A lazy ctor here would race with shutdown: if a worker callback or Lua
    // glue called Get* during/after ~AppController, it would spawn a fresh
    // controller with no joiner, leaving a dangling thread at process exit.
    //
    // The previous lazy-fallback was originally added because Initialize could
    // throw on a bad provider, but the try/catch in Initialize already swallows
    // the exception and leaves aiAssistant_ null — at which point
    // HasAiAssistantController() correctly returns false and callers handle it.
    return *impl_->aiAssistant_; // pre-condition: HasAiAssistantController() == true
}
#endif

void AppController::AddAiContext(const AiContextBlock& block) {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        impl_->aiAssistant_->AddAiContext(block);
    }
#else
    (void)block;
#endif
}

void AppController::ClearAiContext() {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        impl_->aiAssistant_->ClearAiContext();
    }
#endif
}

std::vector<AiContextBlock> AppController::GetAiContext() const {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        return impl_->aiAssistant_->GetAiContext();
    }
#endif
    return {};
}

void AppController::PromptAi(const std::string& prompt) {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        // CPP_CODE_AUDIT.md #33 (PromptAi turns always discarded): route through the SAME
        // `g_ui.assistantTurnGen` counter the chat panel's own Send path bumps, via the
        // GetGlobalAiAssistantUiState() seam — not a process-local counter seeded far above
        // any value assistantTurnGen ever reaches. That old seeding (1<<32) meant the
        // stale-turn gate (`ui->AssistantTurnGen() != turnGen`) was unconditionally true for
        // every PromptAi turn, so every delta/final/error callback was silently dropped
        // after spending real API quota — including wiping the visible chat if a delta
        // landed mid-stream. Mirrors SmatchetAiAssistantUi.cpp's `++d.assistantTurnGen` /
        // `--d.assistantTurnGen` bump-then-rollback-on-rejection shape.
        //
        // Threading: same pre-existing Phase B contract as `ai.*`'s other Lua-glue calls
        // (see AppController_LuaBindings.cpp's "Threading expectation" comment above
        // LuaAiAddContextGlue) — PromptAi is expected to run on the UI thread; a
        // background-worker Lua script calling it races this write against the panel's
        // own `++d.assistantTurnGen` the same way it already races `luaContext_`. Not a
        // new risk introduced here, and no shipped script (SmatchetHooks.lua) calls
        // `ai.prompt` off the UI thread today.
        IAiAssistantUiState& uiState = GetGlobalAiAssistantUiState();
        const uint64_t turnGen = uiState.BumpAssistantTurnGen();
        const bool accepted = impl_->aiAssistant_->Submit(turnGen, prompt, impl_->aiAssistant_->GetAiContext());
        if (!accepted) {
            uiState.RollbackAssistantTurnGen();
            LOG_WARN("AppController::PromptAi: Submit rejected (no live AI provider or shutting down)");
        }
    }
#else
    (void)prompt;
#endif
}

smatchet::cmd::CommandRegistry& AppController::Commands() {
    if (!commandRegistry_) {
        // Lazy fallback — caller invoked us before Initialize (tests, embedded hosts).
        commandRegistry_ = std::make_unique<smatchet::cmd::CommandRegistry>();
    }
    return *commandRegistry_;
}

const smatchet::cmd::CommandRegistry& AppController::Commands() const {
    if (!commandRegistry_) {
        const_cast<AppController*>(this)->commandRegistry_ = std::make_unique<smatchet::cmd::CommandRegistry>();
    }
    return *commandRegistry_;
}

smatchet::cmd::ScenarioRunner& AppController::Scenarios() {
    if (!scenarioRunner_) {
        scenarioRunner_ = std::make_unique<smatchet::cmd::ScenarioRunner>();
    }
    return *scenarioRunner_;
}

const smatchet::cmd::ScenarioRunner& AppController::Scenarios() const {
    if (!scenarioRunner_) {
        const_cast<AppController*>(this)->scenarioRunner_ = std::make_unique<smatchet::cmd::ScenarioRunner>();
    }
    return *scenarioRunner_;
}

std::string AppController::ResolveLuaScriptPath(const std::string& filename) const {

    if (filename.empty() || filename.find("..") != std::string::npos || filename.find(':') != std::string::npos ||

        (!filename.empty() && (filename[0] == '/' || filename[0] == '\\'))) {

        LOG_WARN("ResolveLuaScriptPath: blocked suspicious script path=%s", filename.c_str());

        return std::string();
    }

    if (!luaScriptsDirectory_.empty()) {

        return luaScriptsDirectory_ + filename;
    }

    return std::string("Scripts/") + filename;
}

std::vector<std::string> AppController::ListLuaScriptFiles() const {

    namespace fs = ghc::filesystem;

    std::vector<std::string> out;

    try {

        std::error_code ec;

        fs::path root;

        if (!luaScriptsDirectory_.empty()) {

            root = fs::path(luaScriptsDirectory_);

        } else {

            root = fs::path("Scripts");
        }

        if (!fs::is_directory(root, ec)) {

            return out;
        }

        fs::directory_iterator it(root, ec);
        if (ec) {
            LOG_WARN("ListLuaScriptFiles: failed to enumerate %s: %s", root.string().c_str(), ec.message().c_str());
            return out;
        }
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {

            if (ec) {

                break;
            }

            const auto& ent = *it;
            if (!ent.is_regular_file(ec)) {

                continue;
            }

            const std::string fname = ent.path().filename().string();

            if (fname.size() < 5) {

                continue;
            }

            std::string ext = fname.substr(fname.size() - 4);

            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (ext != ".lua") {

                continue;
            }

            out.push_back(fname);
        }

        std::sort(out.begin(), out.end());

        out.erase(std::unique(out.begin(), out.end()), out.end());

    } catch (const std::exception& ex) {

        LOG_WARN("ListLuaScriptFiles: exception (returning partial/empty): %s", ex.what());

        out.clear();

    } catch (...) {

        LOG_WARN("ListLuaScriptFiles: unknown exception (returning empty).");

        out.clear();
    }

    return out;
}

std::string AppController::ResolveFieldIconAssetPath(const std::string& pathOrUrl) const {

    namespace fs = ghc::filesystem;

    {
        auto cit = fieldIconAssetPathCache_.find(pathOrUrl);
        if (cit != fieldIconAssetPathCache_.end()) {
            return cit->second;
        }
    }

    const std::string t = TrimCopyAsciiWhitespace(pathOrUrl);

    if (t.empty()) {

        return std::string();
    }

    if (t.rfind("https://", 0) == 0 || t.rfind("http://", 0) == 0) {

        if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
            fieldIconAssetPathCache_.clear();
        }
        fieldIconAssetPathCache_.emplace(pathOrUrl, t);
        return t;
    }

    std::error_code ec;

    auto isAllowedPath = [&](const fs::path& absPath) -> bool {
        return FieldIconPathIsAllowed(absPath.string(), luaScriptsDirectory_);
    };

    fs::path inp(t);

    if (!inp.is_absolute()) {

        if (luaScriptsDirectory_.empty()) {

            return std::string();
        }

        std::string rel = t;

        if (rel.size() >= 7 && FieldIconHasCaseInsensitivePrefix(rel, "Scripts")) {

            rel = rel.size() == 7 ? std::string() : rel.substr(8);
        }

        const fs::path combined = fs::path(luaScriptsDirectory_) / fs::path(rel);

        const fs::path absRel = fs::weakly_canonical(combined, ec);

        std::string out;
        if (!ec && isAllowedPath(absRel)) {
            out = absRel.string();
        }
        if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
            fieldIconAssetPathCache_.clear();
        }
        fieldIconAssetPathCache_.emplace(pathOrUrl, out);
        return out;
    }

    const fs::path abs = fs::weakly_canonical(inp, ec);

    std::string out;
    if (!ec && isAllowedPath(abs)) {
        out = abs.string();
    }
    if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
        fieldIconAssetPathCache_.clear();
    }
    fieldIconAssetPathCache_.emplace(pathOrUrl, out);
    return out;
}

std::string AppController::GetAutomationScriptContent() {

    std::string path = ResolveLuaScriptPath("Automation.lua");

    if (path.empty())
        return "";

    std::ifstream ifs(path);

    if (!ifs.is_open())
        return "";

    std::stringstream ss;

    ss << ifs.rdbuf();

    return ss.str();
}

bool AppController::SaveAutomationScriptContent(const std::string& content, std::string& outError) {

    std::string path = ResolveLuaScriptPath("Automation.lua");

    if (path.empty()) {

        outError = "Invalid path";

        return false;
    }

    std::ofstream ofs(path, std::ios::trunc);

    if (!ofs.is_open()) {

        outError = "Could not open file for writing: " + path;

        return false;
    }

    ofs << content;

    return true;
}

#if defined(SMATCHET_WITH_AI)
void AppController::LoadAiChatMessages(std::size_t cap, std::vector<AiMessage>& outMessages,
                                       std::vector<std::int64_t>& outIds) const {
    outMessages.clear();
    outIds.clear();
    if (!Cache) {
        return;
    }
    Cache->LoadChatMessages(cap, outMessages, outIds);
}
#endif

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

bool AppController::RecreateLocalCacheDatabase(std::string& outError) {
    outError.clear();
    if (localCacheDbPath_.empty()) {
        outError = "Local cache database path is not set.";
        return false;
    }
    if (shuttingDown_.load()) {
        outError = "Application is shutting down.";
        return false;
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
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->CancelAndJoinActiveStreamingSync();
        focusedContext().ticketSync_->ResetStaleDeletionState();
    }

    Cache.reset();

    std::string removeErr;
    if (!RemoveLocalCacheDbFiles(localCacheDbPath_, removeErr)) {
        try {
            Cache = std::make_unique<LocalCacheManager>(localCacheDbPath_);
        } catch (const std::exception& ex) {
            outError = removeErr + " Failed to reopen database: " + ex.what();
            return false;
        }
        outError = removeErr;
        return false;
    }

    try {
        Cache = std::make_unique<LocalCacheManager>(localCacheDbPath_);
    } catch (const std::exception& ex) {
        outError = std::string("Failed to open new database: ") + ex.what();
        return false;
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
            Cache = std::make_unique<LocalCacheManager>(":memory:");
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

void AppController::ClearLastTrackerTicketSyncWarning() {
    if (connectivity_) {
        connectivity_->ClearLastTicketSyncWarning();
    }
}

TrackerIssueFetchPack AppController::FetchIssuesForActiveView(const TrackerConfig* configOverride,

                                                              const ViewsStore* viewsOverride) {

    TrackerIssueFetchPack pack;

    // Latch via atomic_load: command handlers (MCP / Lua) can call this off the UI thread, so a
    // plain Backend read would race a live SetBackend swap. The shared_ptr also keeps the backend
    // alive across the blocking FetchIssues call (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);
    if (!backend || !Cache) {

        return pack;
    }

    std::lock_guard<std::mutex> lock(g_TrackerIssueFetchMutex);

    pack.Tickets = backend->Reader().FetchIssues(&pack.FullSyncCompleted, configOverride, viewsOverride,
                                                 &pack.FetchError, &pack.Warning);

    return pack;
}

// ApplyIssueFetchPack / CancelAndJoinActiveStreamingSync: moved to TicketSyncService in
// Phase 1A of the item 11 extraction. Thin delegators below.

void AppController::ApplyIssueFetchPack(TrackerIssueFetchPack pack) {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->ApplyIssueFetchPack(std::move(pack));
    }
}

void AppController::CancelAndJoinActiveStreamingSync() {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->CancelAndJoinActiveStreamingSync();
    }
}

void AppController::TickStreamingApply() {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->TickStreamingApply();
    }
}

void AppController::SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride) {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->SyncWithBackend(configOverride, viewsOverride);
    }
}

bool AppController::IsStreamingSyncActive() const {
    return focusedContext().ticketSync_ && focusedContext().ticketSync_->IsActive();
}
