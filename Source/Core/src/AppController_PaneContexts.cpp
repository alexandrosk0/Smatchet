// AppController_PaneContexts.cpp — multi-grid / pane-context cluster extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/appcontroller-service-extraction.md, Slice 2). Holds the per-pane
// GridLiveContext lifecycle + live-sync coordination: focused-context tracking,
// EnsurePaneLiveSyncStarted / SyncPaneWithBackend, TickAllContexts, TickChangeMonitors +
// the membership/ticket-change diff glue, and the hidden-pane eviction / retirement policy.
// Method DECLARATIONS stay in AppController.h; only definitions moved here, so linkage and
// behavior are identical. Includes are curated to the headers the moved bodies reference
// (verified against every free-function call site + type use; GridLiveContext / TrackerConfig /
// CachedTicket / TrackerConnectivityState arrive transitively via AppController.h).
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining AppController pane-context methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on

#include "ConfigManager.h"
#include "GridContextDepsAdapter.h"
#include "GridPaneEvictionPolicy.h"
#include "ITrackerBackend.h"      // per-pane catalog fetch: backend->FieldCatalog()->FetchFieldCatalog
#include "ITrackerFieldCatalog.h" // TrackerFieldCatalogResult + FetchFieldCatalog
#include "LocalCacheManager.h"
#include "PaneSyncKickPolicy.h"
#include "SalientRosterResolve.h"
#include "TicketSyncService.h"
#include "Views.h"

#include "Sync/MembershipDiffPure.h"
#include "Sync/TicketChangeDiffPure.h"
#include "SmatchetTicketChangeNotifications.h"

#include "Logger.h"
#include "UiPerfMonitor.h" // SMATCHET_UI_PERF_SCOPE in TickAllContexts (multi-grid concurrent-sync perf scope)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
/// Hidden-pane grace window before a non-default context is retired (multi-grid Slice 3,
/// plan item 17). Long enough that tab-flipping never churns contexts; short enough that a
/// pane parked behind another all session frees its sync worker + ticket memory.
const std::chrono::milliseconds kHiddenContextGrace(30000);
} // namespace

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
    // Per-pane catalog population (per-pane-catalog-value-read-routing): SyncPaneWithBackend
    // above just created this pane's OWN backend. A cross-backend pane's context catalog is
    // still empty (the same-backend seed in EnsurePaneContextLive didn't apply), so kick an
    // off-thread fetch into ITS context now. The just-recorded generation is captured so the
    // apply drops if a swap/retirement happens mid-flight (#1081). projectKey is derived from
    // the pane's own resolved view JQL by the fetch (empty ≡ unscoped — same as focused refetch).
    populatePaneCatalogAfterSync_(paneId, cfgCopy, std::string(), capturedGeneration);
}

void AppController::populatePaneCatalogAfterSync_(const std::string& paneId, const TrackerConfig& cfgCopy,
                                                  const std::string& projectKey, std::uint64_t capturedGeneration) {
    // UI thread. Resolve the (just-synced) context; a fetch is only worthwhile when it lacks
    // its OWN catalog — a same-backend pane already carries the seeded copy, and re-fetching
    // it here would duplicate the focused catalog's own fetch traffic for no gain.
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return;
    }
    GridLiveContext& ctx = *it->second;
    {
        std::lock_guard<std::mutex> lk(ctx.fieldCatalog.availableFieldsMutex_);
        if (ctx.fieldCatalog.fieldCatalogEverLoaded_) {
            return; // already populated (same-backend seed or a prior fetch) — nothing to do
        }
    }
    // Capture a STRONG handle to the pane's backend (ADR-0012): a live swap must not free it
    // mid-fetch. atomic_load — the shared_ptr instance read cannot race SetBackend's exchange.
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&ctx.Backend);
    if (!backend || !backend->FieldCatalog()) {
        return; // backend not up yet, or a backend without a catalog endpoint (e.g. fixture)
    }
    TrackerConfig cfgForFetch = cfgCopy; // value-capture: the worker must not touch UI-thread cfg
    LaunchBackgroundTask([this, paneId, backend, cfgForFetch, projectKey, capturedGeneration]() mutable {
        /* PILLAR2_WORKER_ONLY */ // est-latency: catalog HTTP fetch off the UI thread
        auto catalogResult = backend->FieldCatalog()->FetchFieldCatalog(cfgForFetch, projectKey);
        if (!catalogResult) {
            // Non-fatal: the pane keeps rendering against the focused-catalog fallback and the
            // next focus/refresh retries. Logged, not swallowed (policy). No context write.
            LOG_WARN("AppController::populatePaneCatalogAfterSync_ fetch failed pane='%s' err=%s", paneId.c_str(),
                     catalogResult.error().Detail.c_str());
            return;
        }
        TrackerFieldCatalogResult catalog = std::move(catalogResult.value());
        mainThreadDispatcher.PostToMainThread([this, paneId, capturedGeneration, fields = std::move(catalog.Fields),
                                               components = std::move(catalog.Components),
                                               issueTypeMeta = std::move(catalog.IssueTypeMeta)]() mutable {
            applyPaneCatalogOnMainThread_(paneId, capturedGeneration, std::move(fields), std::move(components),
                                          std::move(issueTypeMeta));
        });
    });
}

void AppController::applyPaneCatalogOnMainThread_(const std::string& paneId, std::uint64_t capturedGeneration,
                                                  std::vector<TrackerField> fields,
                                                  std::vector<TrackerComponent> components,
                                                  std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta) {
    // UI thread. Re-validate the CAPTURED context (issue #1457 / #1081 discipline): write ONLY
    // when the pane's context still exists AND its backend generation is unchanged since the
    // fetch was kicked. A swap/retirement between kick and apply means this snapshot belongs to
    // a stale backend — dropping it avoids landing a foreign catalog in the live context (the
    // exact class the debt cluster's #975 entry warns about: never re-resolve focus at
    // completion; write through the kick-time context or drop).
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return;
    }
    GridLiveContext& ctx = *it->second;
    if (ctx.backendGeneration_.load() != capturedGeneration) {
        LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' catalog apply dropped — backend generation "
                 "moved mid-fetch (per-pane-catalog-value-read-routing / #1081).",
                 paneId.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(ctx.fieldCatalog.availableFieldsMutex_);
        ctx.fieldCatalog.AvailableFields = std::move(fields);
        ctx.fieldCatalog.AvailableComponents = std::move(components);
        ctx.fieldCatalog.AvailableIssueTypeMeta = std::move(issueTypeMeta);
        ctx.fieldCatalog.fieldCatalogEverLoaded_ = true;
        ctx.fieldCatalog.LastTrackerFieldCatalogError.clear();
        ctx.fieldCatalog.LastTrackerFieldCatalogErrorTransient = false;
    }
    // Bump AFTER the write is visible so a read-routing consumer that observes the new revision
    // also observes the new fields (the read-routing cache keys on this revision — the
    // same-backend staleness invalidation path).
    ctx.fieldCatalog.TrackerFieldCatalogRevision.fetch_add(1);
    LOG_INFO("AppController::applyPaneCatalogOnMainThread_ pane='%s' populated own catalog (%zu fields).",
             paneId.c_str(), static_cast<size_t>(ctx.fieldCatalog.AvailableFields.size()));
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

// --- Per-pane field-catalog READ routing (per-pane-catalog-value-read-routing) --------------
// These resolve THROUGH the pane's OWN GridLiveContext::fieldCatalog, which is populated by:
//   (a) EnsurePaneContextLive's same-backend one-time seed copy (existing), and
//   (b) the cross-backend per-pane catalog fetch kicked from applyPaneSyncKickOnMainThread_
//       (populatePaneCatalogAfterSync_ below), which writes into the CAPTURED context
//       (never fieldCatalog()) under the #1081 kick-time generation check.
// A pane with no live/populated context falls back to the focused catalog — identical to
// today's shared read, so the routing is never a regression, only an upgrade once populated.
std::uint64_t AppController::GetPaneFieldCatalogRevision(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    return (it == gridContexts_.end()) ? 0 : it->second->fieldCatalog.TrackerFieldCatalogRevision.load();
}

bool AppController::IsPaneFieldCatalogPopulated(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    if (it == gridContexts_.end()) {
        return false;
    }
    const GridContextFieldCatalog& cat = it->second->fieldCatalog;
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_); // availableFieldsMutex_ is mutable
    return cat.fieldCatalogEverLoaded_;
}

std::vector<TrackerField> AppController::GetPaneAvailableFields(const std::string& paneId) const {
    std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
    if (it != gridContexts_.end()) {
        const GridContextFieldCatalog& cat = it->second->fieldCatalog;
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_); // availableFieldsMutex_ is mutable
        if (cat.fieldCatalogEverLoaded_) {
            return cat.AvailableFields; // copy under the lock — the fetch worker may rewrite it
        }
    }
    // No populated per-pane catalog: copy the focused catalog (same data the shared read used).
    const GridContextFieldCatalog& focused = fieldCatalog();
    std::lock_guard<std::mutex> lk(focused.availableFieldsMutex_);
    return focused.AvailableFields;
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
        NotifyTicketChanges(changes, paneId);
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

std::vector<std::string> AppController::CollectTicketIdsRetainedByOtherContexts(const GridLiveContext& self) const {
    const std::string selfKey = self.CacheBackendKeyCopy();
    std::vector<GridLiveContext*> others;
    {
        // Issue #1457 lock order: map mutex OUTERMOST, snapshot the pointers, release it BEFORE
        // taking ANY per-context mutex — including backendKeyMutex_ inside CacheBackendKeyCopy,
        // which is why the key filter happens after this scope. Retired contexts leave the map
        // with their ActiveTickets already cleared (RetireHiddenPaneContexts above), so a pane
        // retired between the snapshot and the read contributes nothing rather than dangling.
        std::lock_guard<std::mutex> mapLk(gridContextsMutex_);
        others.reserve(gridContexts_.size());
        for (std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.begin();
             it != gridContexts_.end(); ++it) {
            GridLiveContext* ctx = it->second.get();
            if (ctx != nullptr && ctx != &self) {
                others.push_back(ctx);
            }
        }
    }

    std::vector<std::string> ids;
    for (std::size_t i = 0; i < others.size(); ++i) {
        GridLiveContext& ctx = *others[i];
        // Only contexts in the same cache namespace can collide: stale-deletion is scoped by
        // backend key, so a Plane pane's rows are invisible to a Jira pane's sweep either way.
        if (ctx.CacheBackendKeyCopy() != selfKey) {
            continue;
        }
        std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
        ids.reserve(ids.size() + ctx.ActiveTickets.size());
        for (std::size_t t = 0; t < ctx.ActiveTickets.size(); ++t) {
            ids.push_back(ctx.ActiveTickets[t].id);
        }
    }
    return ids;
}
