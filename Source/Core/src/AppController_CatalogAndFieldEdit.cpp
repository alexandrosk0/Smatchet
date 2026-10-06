#include "AppController.h"
#include "CatalogOfflinePolicyPure.h"
#include "Config/CacheBackendKeyPure.h" // a guarded catalog apply compares the fetch-time tracker
#include "EditMetaCacheService.h"       // editmeta delegators forward to editMeta_ (god-object decomposition Phase 1).
#include "FieldEditPipelineService.h"   // field-edit delegators forward to fieldEdit_ (decomposition Phase 2).
#include "IssueTransitionsCacheService.h" // transitions delegators forward to transitions_.
#include "ITrackerIssueMutations.h" // fan-in Phase 2: AppController.h fwd-decls it now; this TU calls Mutations() methods.
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls Cache-> methods.
#include "LookupPayloadsPure.h"            // lookup_cache kinds + the user roster codec (Pillar 6).
#include "ProjectComponentsCacheService.h" // component delegators forward to components_.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "BackendAuditTrail.h"
#include "FieldEditAuditSource.h"
#include "ConfigManager.h"
#include "Tracker/CollaborationPreconditionPure.h" // shared, unit-tested collaboration preflight + error map
#include "Tracker/CommentBlobFormatPure.h"         // shared Comments-cell tooltip blob formatter
#include "FieldCatalogCache.h"
#include "ITrackerActivity.h"
#include "JiraClient.h"
#include "ProjectResolver.h"
#include "Sync/TicketRosterFilterPure.h" // shared keep-set roster filter (also used by TicketSyncService)
#include "TrackerFieldPayload.h"
#include "TrackerHttpUtils.h"

#include "Logger.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"
#include "TrackerFieldValueUtils.h"

namespace {

// IsSprintField / IsEditableTimetrackingEstimateFieldId / ErrorTextContainsHttpStatus moved into
// FieldEditPipelineService.cpp (god-object decomposition Phase 2) — their only callers were the
// field-edit pipeline methods now living in the service. IsNonEditableTimetrackingFieldId stays:
// SetFieldCatalog / HandleFieldCatalogError below still mark such fields read-only.
bool IsNonEditableTimetrackingFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsNonEditableTimetrackingFieldId(fieldId);
}

// --- Field-catalog apply, written into the catalog the caller latched ---------------------------
// SetFieldCatalog applies into the focused pane's catalog; RefreshFieldCatalog applies into the
// catalog of the pane it fetched for, latched before the fetch. One implementation serves both.

// The catalog helpers below work on a catalog that is not published yet (a fetched result or a
// restored snapshot), so they take no lock: the caller publishes the finished vectors in one write
// under availableFieldsMutex_.

// Adds a synthetic read-only column unless the catalog already has one with that id.
void EnsureSyntheticField(std::vector<TrackerField>& fields, const char* id, const char* name, const char* type) {
    const auto it =
        std::find_if(fields.begin(), fields.end(), [id](const TrackerField& field) { return field.Id == id; });
    if (it != fields.end()) {
        return;
    }
    TrackerField field;
    field.Id = id;
    field.Name = name;
    field.Type = type;
    field.ReadOnly = true;
    fields.push_back(std::move(field));
}

// Jira timetracking fields the grid cannot edit are shown read-only.
void MarkNonEditableTimetrackingReadOnly(std::vector<TrackerField>& fields) {
    for (auto& field : fields) {
        if (IsNonEditableTimetrackingFieldId(field.Id)) {
            field.ReadOnly = true;
        }
    }
}

// The Jira-only fixups every applied catalog gets:
// - drop Jira's legacy system `comment` field (ADF blob, label "Comment"). It duplicated the synthetic
//   `comments` count column in the picker (#1291 follow-up); the blob still rides in per-ticket
//   fieldValues["comment"] and surfaces as the Comments-cell tooltip, so no text is lost;
// - add the synthetic read-only `history` column (#823);
// - add the synthetic read-only `comments` count column, typed "number" like the GitHub catalog's
//   comments field so the shared comments cell renders a count.
void EraseLegacyCommentField(std::vector<TrackerField>& fields) {
    fields.erase(
        std::remove_if(fields.begin(), fields.end(), [](const TrackerField& field) { return field.Id == "comment"; }),
        fields.end());
}
void EnsureHistoryField(std::vector<TrackerField>& fields) { EnsureSyntheticField(fields, "history", "History", ""); }
void EnsureCommentsField(std::vector<TrackerField>& fields) {
    EnsureSyntheticField(fields, "comments", "Comments", "number");
}
void AddJiraCatalogFieldFixups(std::vector<TrackerField>& fields) {
    EraseLegacyCommentField(fields);
    EnsureHistoryField(fields);
    EnsureCommentsField(fields);
}

// The same fixups on a published catalog, one at a time under its lock. `cat` is the caller's latched
// catalog: never re-resolve fieldCatalog() here, or a focus switch could edit another pane's catalog.
void EnsureHistoryFieldIn(GridContextFieldCatalog& cat) {
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    EnsureHistoryField(cat.AvailableFields);
}
void EnsureCommentsFieldIn(GridContextFieldCatalog& cat) {
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    EnsureCommentsField(cat.AvailableFields);
}
void EraseLegacyCommentFieldIn(GridContextFieldCatalog& cat) {
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    EraseLegacyCommentField(cat.AvailableFields);
}

// Drops a worker's catalog write once the pane it fetched for has moved on: its backend was swapped
// or the pane retired (Generation), or its catalog was explicitly cleared, as a tracker switch does
// before the new backend is installed (Epoch). Checked under availableFieldsMutex_ in the same
// critical section as the write. Retirement moves the generation and an explicit clear moves the
// epoch before they write the catalog, so a guarded write either lands first and is overwritten, or
// sees the move and is dropped. Config is the configuration the fetch ran with: it keys the snapshot.
struct CatalogWriteGuard {
    const std::atomic<std::uint64_t>* Generation = nullptr; ///< null: unguarded (UI-thread callers)
    std::uint64_t ExpectedGeneration = 0;
    const std::atomic<std::uint64_t>* Epoch = nullptr; ///< null: unguarded
    std::uint64_t ExpectedEpoch = 0;
    const TrackerConfig* Config = nullptr; ///< null: key the snapshot by the current configuration
    bool Holds() const {
        return (Generation == nullptr || Generation->load() == ExpectedGeneration) &&
               (Epoch == nullptr || Epoch->load() == ExpectedEpoch);
    }
};

// Body of AppController::HandleFieldCatalogError, writing into `cat`.
void HandleFieldCatalogErrorInto(GridContextFieldCatalog& cat, const std::string& error, bool errorTransient,
                                 const std::string& catalogCacheKey, const std::string& backendKey,
                                 const CatalogWriteGuard& guard = CatalogWriteGuard()) {
    const bool catalogPlane = backendKey == "Plane";
    bool hasFieldsNow;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        hasFieldsNow = !cat.AvailableFields.empty();
    }
    // Pillar 6 (offline-first): a failed refresh never clears a catalog the user already has. When
    // memory is empty, restore the local snapshot whatever the error kind; only the banner differs.
    bool snapshotLoaded = false;
    std::string snapErr;
    if (!hasFieldsNow) {
        std::vector<TrackerField> snapFields;
        std::vector<TrackerComponent> snapComponents;
        std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta;
        snapshotLoaded = FieldCatalogCache::TryLoadFieldCatalogSnapshot(catalogCacheKey, snapFields, snapComponents,
                                                                        snapIssueTypeMeta, snapErr);
        if (snapshotLoaded) {
            if (!catalogPlane) {
                MarkNonEditableTimetrackingReadOnly(snapFields);
                AddJiraCatalogFieldFixups(snapFields);
            }
            {
                std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
                if (!guard.Holds()) {
                    return; // the pane moved on; its catalog is no longer this fetch's to restore
                }
                cat.AvailableFields = std::move(snapFields);
                cat.AvailableComponents = std::move(snapComponents);
                cat.AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
            }
            cat.fieldCatalogEverLoaded_ = true;
        }
    }
    // The banner names the configured backend (Jira / Plane / GitHub / Linear), never a hard-coded one.
    const std::string& backendLabel = backendKey;
    using smatchet::catalogoffline::CatalogFailureBanner;
    switch (smatchet::catalogoffline::DecideCatalogFailureBanner(errorTransient, hasFieldsNow, snapshotLoaded,
                                                                 cat.fieldCatalogEverLoaded_)) {
    case CatalogFailureBanner::WarningUsingCached: {
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        const std::string nextWarning =
            "Offline: using cached " + backendLabel + " field catalog. Last fetch failed: " + error;
        if (nextWarning != cat.LastTrackerFieldCatalogWarning) {
            cat.LastTrackerFieldCatalogWarning = nextWarning;
            cat.TrackerFieldCatalogRevision.fetch_add(1);
        }
        LOG_WARN("AppController::SetFieldCatalog transport failure (catalog preserved): %s", error.c_str());
        return;
    }
    case CatalogFailureBanner::WarningRestoredSnapshot:
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning =
            "Offline: restored " + backendLabel + " field catalog from local snapshot. Last fetch failed: " + error;
        LOG_WARN("AppController::SetFieldCatalog transport failure; loaded snapshot err=%s", snapErr.c_str());
        break;
    case CatalogFailureBanner::WarningSessionHadCatalog:
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning =
            "Offline: no field catalog snapshot could be loaded for this tracker context. Last fetch failed: " + error;
        LOG_WARN("AppController::SetFieldCatalog transport failure; no snapshot (session had catalog): %s",
                 error.c_str());
        break;
    case CatalogFailureBanner::ErrorKeepCatalog:
        // Non-retryable (auth / config / parse): the user must act, so show the error banner, but keep
        // the catalog. The grid holds pending edits instead of discarding them.
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogError = error;
        cat.LastTrackerFieldCatalogErrorTransient = false;
        LOG_ERROR("AppController::SetFieldCatalog error (catalog kept): %s", error.c_str());
        break;
    case CatalogFailureBanner::ErrorNoCatalog:
        cat.fieldCatalogEverLoaded_ = false;
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogErrorTransient = errorTransient;
        cat.LastTrackerFieldCatalogError = errorTransient
                                               ? "No cached " + backendLabel + " field catalog available. " +
                                                     (error.empty() ? std::string("Last fetch failed.") : error)
                                               : error;
        LOG_ERROR("AppController::SetFieldCatalog error (no cache): %s", error.c_str());
        break;
    }
    cat.TrackerFieldCatalogRevision.fetch_add(1);
}

// Body of AppController::SetFieldCatalog, writing into `cat`. True when a catalog was applied (the
// caller then raises the tracker-reachable notice); false when `error` was handled instead.
bool ApplyFieldCatalogInto(GridContextFieldCatalog& cat, std::vector<TrackerField> fields,
                           std::vector<TrackerComponent> components,
                           std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error,
                           bool errorTransient, const CatalogWriteGuard& guard = CatalogWriteGuard()) {
    const TrackerConfig cfgSnap = guard.Config != nullptr ? *guard.Config : ConfigManager::Load();
    if (guard.Config != nullptr && smatchet::cache_keys::TrackerCacheBackendKey(cfgSnap) !=
                                       smatchet::cache_keys::TrackerCacheBackendKey(ConfigManager::Load())) {
        // The configured tracker changed while this fetch ran: its result belongs to a tracker the
        // pane is leaving, so it is neither shown nor saved under the new tracker's snapshot key.
        return false;
    }
    const std::string backendKey = ConfigManager::NormalizeViewsBackendKey(cfgSnap.TrackerType);
    const bool catalogPlane = backendKey == "Plane";
    // No legacy global project fields exist. Saves under the unscoped ("") cache key when
    // the caller hasn't pinned a project via SetCurrentCatalogProject(). Per-project refetches
    // (driven by the new-issue draft / picker UI) set that hint so the snapshot lands under
    // the right per-project entry. (A future refactor may thread the project as an explicit
    // parameter on the call chain instead of via this latched hint.)
    // Read cat.currentCatalogProjectKey_ under the lock into a local — SetCurrentCatalogProject /
    // RefreshFieldCatalog write it under cat.availableFieldsMutex_ from other threads, so an unlocked
    // read of the std::string here is a data race.
    std::string projectKeyForCache;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        projectKeyForCache = cat.currentCatalogProjectKey_;
    }
    const std::string catalogCacheKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfgSnap, projectKeyForCache);
    (void)catalogPlane;

    if (!error.empty()) {
        HandleFieldCatalogErrorInto(cat, error, errorTransient, catalogCacheKey, backendKey, guard);
        return false;
    }

    if (!guard.Holds()) {
        return false; // already superseded: skip the snapshot write too
    }
    {
        // The snapshot keeps the raw catalog, saved from this call's own vectors (never from the shared
        // catalog, which the UI thread may be writing); the sweep and fixups are re-applied on restore.
        std::string snapErr;
        const std::string saveBackend = catalogPlane ? std::string("Plane") : std::string("Jira");
        const std::string saveEndpoint =
            catalogPlane ? (cfgSnap.PlaneUrl + std::string("|") + cfgSnap.PlaneWorkspaceSlug) : cfgSnap.Domain;
        if (!FieldCatalogCache::SaveFieldCatalogSnapshot(catalogCacheKey, saveBackend, saveEndpoint, projectKeyForCache,
                                                         cfgSnap.FieldCatalogCacheMaxProjects, fields, components,
                                                         issueTypeMeta, snapErr)) {
            LOG_WARN("AppController::SetFieldCatalog: snapshot save failed: %s", snapErr.c_str());
        }
    }
    if (!catalogPlane) {
        MarkNonEditableTimetrackingReadOnly(fields);
        AddJiraCatalogFieldFixups(fields);
    }
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        if (!guard.Holds()) {
            return false;
        }
        cat.AvailableFields = std::move(fields);
        cat.AvailableComponents = std::move(components);
        cat.AvailableIssueTypeMeta = std::move(issueTypeMeta);
    }
    cat.LastTrackerFieldCatalogError.clear();
    cat.LastTrackerFieldCatalogErrorTransient = false;
    cat.LastTrackerFieldCatalogWarning.clear();
    cat.fieldCatalogEverLoaded_ = true;
    cat.TrackerFieldCatalogRevision.fetch_add(1);
    return true;
}

} // namespace

void AppController::RefreshLocalData() { RefreshLocalDataCheckedImpl_(focusedContext(), nullptr); }

void AppController::RefreshLocalDataCheckedImpl_(GridLiveContext& ctx, const std::uint64_t* capturedBackendGeneration,
                                                 const std::string& admitId) {
    // Latched once: worker callers (offline replay, UpdateTicket's queued saves) must not race the UI
    // thread's atomic_store in RecreateLocalCacheDatabase.
    const std::shared_ptr<LocalCacheManager> cache = std::atomic_load(&Cache);
    if (cache) {
        // Full-table read stays OUTSIDE activeTicketsMutex_ (SQLite I/O under the tickets
        // mutex would block the UI-thread readers, Pillar 2); the generation re-check below
        // is therefore the authoritative gate, taken immediately before the swap-in.
        const std::string cacheKey = ctx.CacheBackendKeyCopy();
        auto latestTickets = cache->GetAllTickets(cacheKey);
        // GetAllTickets returns the WHOLE backend-keyed namespace, which every pane shares
        // (ADR-0018 decision 4) — so a bare replace repopulates this pane with its siblings'
        // rows. Filter down to the ids this pane's own sync recorded. Only the ABSENCE of a
        // recorded set means a true cold start (nothing has synced yet), where the whole
        // namespace IS the best available seed — that is what AppController_Init's bootstrap
        // refresh relies on. A recorded-but-EMPTY set is the retirement tombstone of a pane the
        // user came back to (issue #2063): it must render empty until its own sync lands, not
        // re-leak every sibling pane's rows into its grid.
        std::vector<std::string> owned;
        const bool hasOwned = TryGetPaneOwnedTicketIds(cacheKey, ctx.PaneId, owned);
        if (hasOwned) {
            std::unordered_set<std::string> keep(owned.begin(), owned.end());
            if (!admitId.empty()) {
                keep.insert(admitId); // the row the caller just saved — never filtered out
            }
            const std::size_t dropped = smatchet::RetainTicketsInKeepSet(latestTickets, keep);
            if (dropped != 0) {
                LOG_DEBUG("AppController::RefreshLocalData scoped namespace read to pane '%s' (dropped %zu of %zu "
                          "sibling rows).",
                          ctx.PaneId.c_str(), dropped, dropped + latestTickets.size());
            }
        }
        {
            std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
            // Capture-then-check (issue #1081): the caller latched ctx's backend generation
            // at work-capture time; if the backend was swapped/retired since — including
            // DURING the GetAllTickets read above — this wholesale replace would push the
            // OLD backend's rows into the NEW backend's just-cleared grid. Re-checked under
            // ctx.activeTicketsMutex_ (the SAME context the caller captured from) so the
            // decision is ordered against the swap path's locked clear+publish.
            if (capturedBackendGeneration != nullptr && ctx.backendGeneration_.load() != *capturedBackendGeneration) {
                LOG_INFO("AppController::RefreshLocalData skipped — backend generation moved since capture "
                         "(issue #1081): key='%s' captured=%llu current=%llu",
                         cacheKey.c_str(), static_cast<unsigned long long>(*capturedBackendGeneration),
                         static_cast<unsigned long long>(ctx.backendGeneration_.load()));
                return;
            }
            ctx.ActiveTickets = std::move(latestTickets);
            ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
        }
        // The admitted row now belongs to this pane for good — record it, or the NEXT refresh
        // (which re-reads the recorded set) would filter the freshly-saved ticket back out.
        // Appended ATOMICALLY (issue #2050): this method is worker-invoked, so writing back the
        // `owned` copy snapshotted before the GetAllTickets read above would silently revert a
        // PublishOwnedTicketIds that landed in that window — dropping the freshly-synced rows
        // from the grid AND unpinning them from a sibling's stale deletion. AddPaneOwnedTicketId
        // re-reads the entry under the store's lock and appends, so both writes survive.
        if (!admitId.empty() && hasOwned) {
            AddPaneOwnedTicketId(cacheKey, ctx.PaneId, admitId);
        }
        PruneEditMetaCacheToActiveTickets();
        ctx.ActiveTicketsRevision.fetch_add(1);
        // Lua window dirty-bump on ticket data change; the only other site is UpdateTicket's
        // in-memory patch (PatchActiveTicketInPlace_), whose queued save re-reads through here.
        // Stub build turns this into a no-op.
        NotifyLuaTicketDataChanged();
    }
}

void AppController::UpdateTicket(const CachedTicket& ticket) {
    GridLiveContext& ctx = focusedContext();
    UpdateTicketInContext_(ctx, ctx.backendGeneration_.load(), ticket);
}

void AppController::UpdateTicketInContext_(GridLiveContext& ctx, std::uint64_t capturedGeneration,
                                           const CachedTicket& ticket) {
    const std::shared_ptr<LocalCacheManager> cache = std::atomic_load(&Cache);
    if (!cache) {
        return;
    }
    // Latch key + generation TOGETHER (issue #1081): reading the key here and re-reading it inside the
    // refresh raced a backend swap — the save landed under the OLD key while the refresh read the NEW key
    // (proven TOCTOU: a Jira row contaminating the GitHub namespace). A swap after this check is benign:
    // the write still lands under the CAPTURED key, which is where the row belongs, and the checked
    // patch and refresh drop their grid replace.
    const std::string capturedKey = ctx.CacheBackendKeyCopy();
    if (ctx.backendGeneration_.load() != capturedGeneration) {
        // WARN, not INFO: callers reach UpdateTicket after the backend mutation already succeeded
        // upstream — dropping the local save leaves a stale row until the old backend's next sync.
        LOG_WARN("AppController::UpdateTicket skipped — backend swapped during key latch (issue #1081): "
                 "key='%s' ticket='%s' generation=%llu",
                 capturedKey.c_str(), ticket.id.c_str(), static_cast<unsigned long long>(capturedGeneration));
        return;
    }
    // Pillar 2: callers run on the UI thread, so the grid shows the change now and the SQLite write plus
    // the full-table re-read run on a worker, in call order (two quick edits of one ticket never land
    // reversed).
    PatchActiveTicketInPlace_(ctx, capturedGeneration, ticket);
    const std::uint64_t saveSeq = ctx.ticketSaveSeq_.fetch_add(1) + 1;
    // A context is never freed while the app runs (retired ones become husks), and the queued cache is
    // held weakly so a queued save never keeps a replaced database file open.
    GridLiveContext* const ctxPtr = &ctx;
    const std::weak_ptr<LocalCacheManager> queuedCache = cache;
    const std::string launchError =
        ticketSaves_.Post([this, ctxPtr, capturedGeneration, capturedKey, ticket, queuedCache, saveSeq]() {
            SaveTicketThenRefresh_(*ctxPtr, capturedGeneration, capturedKey, ticket, queuedCache, saveSeq);
        });
    if (!launchError.empty()) {
        // The save stays queued and runs with the next one; the grid already shows the change.
        LOG_ERROR("AppController::UpdateTicket could not start the cache write worker ticket='%s' err=%s",
                  ticket.id.c_str(), launchError.c_str());
    }
}

void AppController::PatchActiveTicketInPlace_(GridLiveContext& ctx, std::uint64_t capturedGeneration,
                                              const CachedTicket& ticket) {
    {
        std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
        if (ctx.backendGeneration_.load() != capturedGeneration) {
            return;
        }
        const auto it = std::find_if(ctx.ActiveTickets.begin(), ctx.ActiveTickets.end(),
                                     [&ticket](const CachedTicket& row) { return row.id == ticket.id; });
        if (it == ctx.ActiveTickets.end()) {
            return; // not shown in this pane (any more): the queued refresh admits it if it belongs here
        }
        *it = ticket;
        ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
    }
    ctx.ActiveTicketsRevision.fetch_add(1);
    NotifyLuaTicketDataChanged();
}

void AppController::SaveTicketThenRefresh_(GridLiveContext& ctx, std::uint64_t capturedGeneration,
                                           const std::string& capturedKey, const CachedTicket& ticket,
                                           const std::weak_ptr<LocalCacheManager>& queuedCache, std::uint64_t saveSeq) {
    const std::shared_ptr<LocalCacheManager> cache = queuedCache.lock();
    if (!cache || cache != std::atomic_load(&Cache)) {
        // The database was recreated since: it resyncs from the tracker, and this row with it.
        LOG_INFO("AppController::UpdateTicket dropped a queued cache write — the local cache was replaced "
                 "ticket='%s'",
                 ticket.id.c_str());
        return;
    }
    // A SQLite write can throw — most plausibly BUSY once RunWriteTxnWithBusyRetry's deadline is spent.
    // Report it and keep the row as the grid shows it; the offline queue and the next sync already
    // expect a stale cache row. (Log-only: this domain TU cannot reach the grid's error state, which
    // lives in Ui/.)
    try {
        cache->SaveTicket(capturedKey, ticket);
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::UpdateTicket local cache write failed key='%s' ticket='%s' err=%s",
                  capturedKey.c_str(), ticket.id.c_str(), ex.what());
        return;
    }
    if (ctx.ticketSaveSeq_.load() != saveSeq) {
        // A newer save for this pane is queued and its re-read shows both rows. That re-read admits only its own
        // id, so record this row as the pane's now, as the skipped re-read would have (a no-op on a pane with no
        // recorded set yet, which shows the whole namespace anyway).
        if (ctx.backendGeneration_.load() == capturedGeneration) {
            AddPaneOwnedTicketId(capturedKey, ctx.PaneId, ticket.id);
        }
        return;
    }
    // Re-read checked against the SAME latched ctx (MEDIUM-1). `ticket.id` is admitted explicitly: the
    // pane-scoping filter works off the last SYNCED id set, which may not contain this row yet.
    try {
        RefreshLocalDataCheckedImpl_(ctx, &capturedGeneration, ticket.id);
    } catch (const std::exception& ex) {
        // The grid keeps its patched row; the next refresh re-reads the cache.
        LOG_ERROR("AppController::UpdateTicket grid re-read failed key='%s' ticket='%s' err=%s", capturedKey.c_str(),
                  ticket.id.c_str(), ex.what());
    }
}

GridLiveContext* AppController::liveContextForTarget_(const PendingActionTarget& target) const {
    if (target.PaneId.empty()) {
        return nullptr;
    }
    GridLiveContext* ctx = nullptr;
    {
        std::lock_guard<std::mutex> mapLk(gridContextsMutex_);
        const auto it = gridContexts_.find(target.PaneId);
        ctx = (it != gridContexts_.end()) ? it->second.get() : nullptr;
    }
    if (ctx == nullptr || ctx->backendGeneration_.load() != target.BackendGeneration) {
        return nullptr;
    }
    return ctx;
}

std::shared_ptr<const std::vector<CachedTicket>>
AppController::TicketsSnapshotForTarget_(const PendingActionTarget& target) const {
    const GridLiveContext* ctx = liveContextForTarget_(target);
    return ctx != nullptr ? ctx->ActiveTicketsSnapshot() : nullptr;
}

void AppController::UpdateTicketForTarget_(const PendingActionTarget& target, const CachedTicket& ticket) {
    GridLiveContext* ctx = liveContextForTarget_(target);
    if (ctx == nullptr) {
        LOG_INFO("AppController::UpdateTicketForTarget_ pane '%s' was closed or switched tracker; ticket='%s' not "
                 "saved locally",
                 target.PaneId.c_str(), ticket.id.c_str());
        return;
    }
    UpdateTicketInContext_(*ctx, target.BackendGeneration, ticket);
}

bool AppController::RefreshFieldCatalog(const TrackerConfig& cfg) { return RefreshFieldCatalog(cfg, std::string()); }

bool AppController::RefreshFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey) {
    // Runs on a background worker (the new-issue draft / picker catalog refresh). Latch the focused
    // context ONCE and both read its backend and write its catalog through that latch: resolving focus
    // again (a second focusedContext(), or SetFieldCatalog's fieldCatalog() at completion) would pair
    // one pane's backend with another pane's catalog, or land the result in whichever pane is focused
    // when the fetch finishes. The retired-context husk graveyard keeps the latched context valid for
    // the life of this call. The write guard (checked under the catalog mutex at each write) drops a
    // result whose pane switched tracker, was retired, or had its catalog cleared during the fetch.
    GridLiveContext& ctx = focusedContext();
    CatalogWriteGuard guard;
    guard.Generation = &ctx.backendGeneration_;
    guard.ExpectedGeneration = ctx.backendGeneration_.load();
    guard.Epoch = &ctx.fieldCatalog.CatalogEpoch;
    guard.ExpectedEpoch = ctx.fieldCatalog.CatalogEpoch.load();
    guard.Config = &cfg;
    // Strong handle: a live tracker switch (SetBackend on the UI thread) must not free the backend
    // mid-FetchFieldCatalog — the FieldCatalog object dereferenced below lives inside it (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&ctx.Backend);
    GridContextFieldCatalog& cat = ctx.fieldCatalog;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        cat.currentCatalogProjectKey_ = projectKey;
    }
    if (!backend) {
        // config-class: non-transient default
        ApplyFieldCatalogInto(cat, {}, {}, {}, "Tracker backend is not initialized.", false, guard);
        return false;
    }

    TrackerFieldCatalogResult catalog;
    std::string error;
    bool errorTransient = false;
    bool ok = false;
    if (backend->FieldCatalog()) {
        auto catalogResult = backend->FieldCatalog()->FetchFieldCatalog(cfg, projectKey);
        ok = static_cast<bool>(catalogResult);
        if (ok) {
            catalog = std::move(catalogResult.value());
        } else {
            error = catalogResult.error().Detail;
            // N12 item 13b: classify at the flatten seam from the structured kind.
            errorTransient = catalogResult.error().IsRetryable();
        }
    }
    if (!ok) {
        if (!guard.Holds()) {
            // A stale failure must not set the error banner of a pane that has moved on.
            LOG_INFO("AppController::RefreshFieldCatalog: pane '%s' moved on during the fetch; its error was dropped",
                     ctx.PaneId.c_str());
            return false;
        }
        ApplyFieldCatalogInto(cat, {}, {}, {}, error, errorTransient, guard);
        LOG_ERROR("AppController::RefreshFieldCatalog failed: %s", error.c_str());
        return false;
    }

    if (!ApplyFieldCatalogInto(cat, std::move(catalog.Fields), std::move(catalog.Components),
                               std::move(catalog.IssueTypeMeta), std::string(), false, guard)) {
        // With no error passed in, false means the guard dropped the write.
        LOG_INFO("AppController::RefreshFieldCatalog: pane '%s' switched tracker, was retired or had its catalog "
                 "cleared during the fetch; its result was dropped",
                 ctx.PaneId.c_str());
        return false;
    }
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::FetchFieldCatalog(const TrackerConfig& cfg, TrackerFieldCatalogResult& outCatalog,
                                      std::string& outError) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outCatalog = TrackerFieldCatalogResult{};
    outError.clear();
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    // Project is per-operation (there is no global project key). This convenience overload is
    // called by config-time probes that don't pin a project; backend returns the unscoped catalog.
    if (!backend->FieldCatalog()) {
        outError = "FetchFieldCatalog is not supported by this backend.";
        return false;
    }
    auto result = backend->FieldCatalog()->FetchFieldCatalog(cfg, std::string());
    if (!result) {
        outError = result.error().Detail;
        return false;
    }
    outCatalog = std::move(result.value());
    return true;
}

bool AppController::FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                                      TrackerFieldCatalogResult& outCatalog, std::string& outError) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outCatalog = TrackerFieldCatalogResult{};
    outError.clear();
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (!backend->FieldCatalog()) {
        outError = "FetchFieldCatalog is not supported by this backend.";
        return false;
    }
    auto result = backend->FieldCatalog()->FetchFieldCatalog(cfg, projectKey);
    if (!result) {
        outError = result.error().Detail;
        return false;
    }
    outCatalog = std::move(result.value());
    return true;
}

std::string AppController::BuildIssueBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    return backend ? backend->Reader().BuildBrowseUrl(cfg, issueKey) : std::string();
}

std::string AppController::ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                               const std::string& value) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (!backend) {
        return value;
    }
    return backend->Reader().ResolveDisplayValue(fieldId, field, value);
}

std::string AppController::BuildJqlSearchUrl(const TrackerConfig& cfg, const std::string& jql) {
    if (cfg.Domain.empty() || jql.empty()) {
        return std::string();
    }
    return NormalizeBaseUrl(cfg.Domain) + "/issues/?jql=" + UrlEncode(jql);
}

void AppController::SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                                    const std::string& error, bool errorTransient) {
    SetFieldCatalog(std::move(fields), std::move(components), {}, error, errorTransient);
}

void AppController::SetCurrentCatalogProject(const std::string& projectKey) {
    GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    cat.currentCatalogProjectKey_ = projectKey;
}

void AppController::SetAvailableUsers(std::vector<TrackerUser> users) {
    GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    cat.AvailableUsers = std::move(users);
}

std::shared_ptr<ILookupCache> AppController::LookupCacheShared() const { return std::atomic_load(&Cache); }

void AppController::SaveAvailableUsersForOfflineAsync(const std::string& cacheBackendKey,
                                                      std::string usersPayloadJson) {
    const std::shared_ptr<LocalCacheManager> store = std::atomic_load(&Cache);
    if (!store || cacheBackendKey.empty() || usersPayloadJson.empty()) {
        return;
    }
    try {
        LaunchBackgroundTask([store, cacheBackendKey, payload = std::move(usersPayloadJson)]() {
            store->UpsertLookup(cacheBackendKey, smatchet::lookup::kUsersKind, smatchet::lookup::kUsersKey, payload);
        });
    } catch (const std::exception& ex) {
        LOG_WARN("AppController: saving the user roster did not start: %s", ex.what());
    }
}

void AppController::SeedAvailableUsersFromStoreAsync() {
    GridLiveContext& ctx = focusedContext(); // latch once: the apply below targets this pane
    {
        std::lock_guard<std::mutex> lk(ctx.fieldCatalog.availableFieldsMutex_);
        if (!ctx.fieldCatalog.AvailableUsers.empty()) {
            return;
        }
    }
    const std::shared_ptr<LocalCacheManager> store = std::atomic_load(&Cache);
    const std::string backendKey = ctx.CacheBackendKeyCopy();
    if (!store || backendKey.empty()) {
        return;
    }
    // Dangle-safety: a retired context stays alive as a husk in retiredContexts_ until ~AppController,
    // and the backend generation + namespace re-check below drops an apply after a tracker swap.
    GridLiveContext* ctxPtr = &ctx;
    const std::uint64_t generation = ctx.backendGeneration_.load();
    try {
        LaunchBackgroundTask([this, store, backendKey, ctxPtr, generation]() {
            LookupCacheRow row;
            auto users = std::make_shared<std::vector<TrackerUser>>();
            if (!store->TryGetLookup(backendKey, smatchet::lookup::kUsersKind, smatchet::lookup::kUsersKey, row) ||
                !smatchet::lookup::ParseUsers(row.PayloadJson, *users) || users->empty()) {
                return;
            }
            // AvailableUsers is read by reference on the UI thread, so it is only written there.
            PostToMainThread([ctxPtr, generation, backendKey, users]() {
                if (ctxPtr->backendGeneration_.load() != generation || ctxPtr->CacheBackendKeyCopy() != backendKey) {
                    return; // the pane switched tracker meanwhile
                }
                GridContextFieldCatalog& cat = ctxPtr->fieldCatalog;
                std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
                if (cat.AvailableUsers.empty()) { // never override a live roster
                    cat.AvailableUsers = std::move(*users);
                }
            });
        });
    } catch (const std::exception& ex) {
        LOG_WARN("AppController: loading the saved user roster did not start: %s", ex.what());
    }
}

void AppController::SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                                    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error,
                                    bool errorTransient) {
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would lock context A's mutex while mutating context B (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    if (fields.empty() && components.empty() && issueTypeMeta.empty() && error.empty()) {
        // An explicit clear (the reset a tracker switch starts with) supersedes every catalog fetch
        // already in flight for this pane, before the new backend is even installed.
        cat.CatalogEpoch.fetch_add(1);
    }
    if (ApplyFieldCatalogInto(cat, std::move(fields), std::move(components), std::move(issueTypeMeta), error,
                              errorTransient)) {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
}

void AppController::HandleFieldCatalogError(const std::string& error, bool errorTransient,
                                            const std::string& catalogCacheKey, const std::string& backendKey) {
    // Latch the catalog once (Pillar 3), as SetFieldCatalog does.
    HandleFieldCatalogErrorInto(fieldCatalog(), error, errorTransient, catalogCacheKey, backendKey);
}

const TrackerField* AppController::FindFieldById(const std::string& fieldId) const {
    const GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    const auto it = std::find_if(cat.AvailableFields.begin(), cat.AvailableFields.end(),
                                 [&](const TrackerField& field) { return field.Id == fieldId; });
    return it == cat.AvailableFields.end() ? nullptr : &(*it);
}

void AppController::EnsureCatalogHistoryField(GridContextFieldCatalog& cat) { EnsureHistoryFieldIn(cat); }

void AppController::EnsureCatalogCommentsField(GridContextFieldCatalog& cat) { EnsureCommentsFieldIn(cat); }

void AppController::EraseCatalogLegacyCommentField(GridContextFieldCatalog& cat) { EraseLegacyCommentFieldIn(cat); }

// FieldEditSupportsOfflineQueue + the field-edit pipeline (CommitOrQueue and its network / offline-
// prepare helpers, ApplyFieldEditResult) live in FieldEditPipelineService (god-object decomposition
// Phase 2). Only thin public delegators remain here; see the delegator block below. The editmeta cache
// methods moved earlier (Phase 1, EditMetaCacheService).

ProjectComponentsLookup AppController::GetComponentOptionsForProject(const std::string& projectKey) const {
    return components_ ? components_->GetComponentOptions(projectKey) : ProjectComponentsLookup();
}

bool AppController::EnsureProjectComponentsLoaded(const std::string& projectKey) {
    return components_ && components_->EnsureComponentsLoaded(projectKey);
}

bool AppController::IsFieldCatalogScopedToProject(const std::string& projectKey) const {
    if (projectKey.empty()) {
        return false;
    }
    const GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    return ToLowerAsciiCopy(cat.currentCatalogProjectKey_) == ToLowerAsciiCopy(projectKey);
}

bool AppController::FieldCatalogLacksProjectScope() const {
    // Jira only: Plane resolves its project per operation and GitHub / Linear have no
    // createmeta-shaped scoping, so an empty key means nothing for them.
    const std::shared_ptr<ITrackerBackend> backend = BackendShared();
    if (!backend || backend->Connectivity().GetTrackerType() != "Jira") {
        return false;
    }
    const GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    return cat.fieldCatalogEverLoaded_ && cat.currentCatalogProjectKey_.empty();
}

// Editmeta-cache delegators — forward to `editMeta_` (EditMetaCacheService, god-object
// decomposition Phase 1). The service owns `editMetaMutex_` + its three containers + the
// service-private ResolveIssueTypeKeyForIssue + WarmIssueTypeEditMetaWorker. `editMeta_` is
// constructed eagerly in Initialize, so it is non-null for every call after startup.

bool AppController::CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                                         const TrackerField* fieldMeta, const std::string* issueTypeKeyOverride) const {
    return editMeta_->CanEditFieldForIssue(issueId, fieldId, fieldMeta, issueTypeKeyOverride);
}

VoidResult AppController::EnsureIssueEditMetaLoaded(const std::string& issueId, const std::string* issueTypeKeyOverride,
                                                    const TrackerConfig* configSnapshot) {
    return editMeta_->EnsureIssueEditMetaLoaded(issueId, issueTypeKeyOverride, configSnapshot);
}

VoidResult AppController::RefreshIssueEditMeta(const std::string& issueId, const std::string* issueTypeKeyOverride) {
    return editMeta_->RefreshIssueEditMeta(issueId, issueTypeKeyOverride);
}

void AppController::InvalidateIssueEditMeta(const std::string& issueId) { editMeta_->InvalidateIssueEditMeta(issueId); }

void AppController::PruneEditMetaCacheToActiveTickets() { editMeta_->PruneEditMetaCacheToActiveTickets(); }

void AppController::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker) {
    // The same warm fills the per-project component options, so each row's components editor has them.
    if (components_) {
        components_->WarmForActiveTicketsAsync(trackerCfgForWorker);
    }
    editMeta_->WarmIssueTypeEditMetaAtStartAsync(std::move(trackerCfgForWorker));
}

void AppController::WarmIssueEditMetaAsync(const std::string& issueId) { editMeta_->WarmIssueEditMetaAsync(issueId); }

// Issue-transitions delegators — forward to `transitions_` (IssueTransitionsCacheService).
// The service owns the per-issue transitions cache and load/invalidate methods.
// `transitions_` is constructed eagerly in Initialize, so it is non-null for every call after startup.

struct TransitionsLookup AppController::GetAvailableTransitionsForIssue(const TransitionsQuery& query) const {
    return transitions_->GetAvailableTransitions(query);
}

void AppController::EnsureIssueTransitionsLoaded(const TransitionsQuery& query) const {
    transitions_->EnsureIssueTransitionsLoaded(query);
}

void AppController::InvalidateIssueTransitions(const std::string& issueId) {
    transitions_->InvalidateIssueTransitions(issueId);
}

// Field-edit pipeline delegators — forward to `fieldEdit_` (FieldEditPipelineService, god-object
// decomposition Phase 2). The service owns the network / offline-prepare helpers (all service-private).
// `fieldEdit_` is constructed eagerly in Initialize, so it is non-null for every call after startup.

PendingActionSubmitResult AppController::SubmitFieldEditOrQueue(const PendingActionTarget& target,
                                                                const std::string& issueId, const TrackerField& field,
                                                                const std::vector<std::string>& values) {
    FieldEditCommitRequest req;
    req.IssueId = issueId;
    req.Field = field;
    req.Values = values;
    req.Target = target.PaneId.empty() ? LatchPendingActionTarget() : target;
    // Conflict base + estimate / issue-type snapshots from the ticket as the edited pane shows it.
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = TicketsSnapshotForTarget_(req.Target);
    if (tickets) {
        const auto ticketIt = std::find_if(tickets->begin(), tickets->end(),
                                           [&issueId](const CachedTicket& ticket) { return ticket.id == issueId; });
        if (ticketIt != tickets->end()) {
            FieldEditPipelineService::CaptureTicketSnapshots(*ticketIt, true, req);
        }
    }
    FieldEditCommitOutcome outcome = fieldEdit_->CommitOrQueue(req);

    PendingActionSubmitResult result;
    result.QueueId = outcome.QueueId;
    result.QueuedAfterNetworkFailure = outcome.QueuedAfterTransportFailure;
    result.Error = outcome.Error;
    if (outcome.Kind == FieldEditCommitKind::Failed) {
        return result;
    }
    result.K = outcome.Kind == FieldEditCommitKind::QueuedOffline ? PendingActionSubmitResult::Kind::Queued
                                                                  : PendingActionSubmitResult::Kind::Sent;
    // The local update and the probe request touch UI-thread state: run them there.
    const PendingActionTarget applyTarget = req.Target;
    const bool probeNow = outcome.QueuedAfterTransportFailure;
    std::function<void()> applyOnUi = [this, applyTarget, issueId, apply = std::move(outcome.Apply), probeNow]() {
        const VoidResult applied = fieldEdit_->ApplyFieldEditResult(applyTarget, issueId, apply);
        if (!applied.has_value()) {
            LOG_WARN("AppController::SubmitFieldEditOrQueue local update failed issue=%s: %s", issueId.c_str(),
                     applied.error().c_str());
        }
        if (probeNow) {
            RequestTrackerProbeNow();
        }
    };
    if (IsOnUiThread()) {
        applyOnUi();
    } else {
        PostToMainThread(std::move(applyOnUi));
    }
    return result;
}

FieldEditCommitOutcome AppController::CommitOrQueueFieldEdit(const FieldEditCommitRequest& req) {
    return fieldEdit_->CommitOrQueue(req);
}

VoidResult AppController::ApplyFieldEditResult(const PendingActionTarget& target, const std::string& issueId,
                                               const FieldEditResult& result) {
    return fieldEdit_->ApplyFieldEditResult(target, issueId, result);
}
template <typename T>
Result<T, TrackerError> AppController::callFocusedCollaboration_(
    const char* what, const std::string& subject,
    const std::function<Result<T, TrackerError>(ITrackerCollaboration&)>& call) const {
    using CallResult = Result<T, TrackerError>;
    // Latch: a live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012).
    const std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);
    if (!backend) {
        // Backend-agnostic wording (issue #2065): these calls are reached on Linear / GitHub / Plane too.
        return CallResult::Err(TrackerErrorInvalidRequest("Tracker backend is not initialized."));
    }
    ITrackerCollaboration* collaboration = backend->Collaboration();
    if (!collaboration) {
        return CallResult::Err(TrackerErrorInvalidRequest("Tracker backend does not support collaboration features."));
    }
    CallResult outcome = call(*collaboration);
    if (!outcome.has_value()) {
        LOG_ERROR("AppController::%s failed for %s kind=%s err=%s", what, subject.c_str(),
                  ToString(outcome.error().Kind), outcome.error().Detail.c_str());
        return outcome;
    }
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return outcome;
}

Result<std::vector<TrackerUser>> AppController::FetchIssueWatchers(const std::string& issueKey) const {
    return smatchet::collab::CollaborationResultToResult<std::vector<TrackerUser>>(FetchIssueWatchersTyped(issueKey));
}

Result<std::vector<TrackerUser>, TrackerError>
AppController::FetchIssueWatchersTyped(const std::string& issueKey) const {
    return callFocusedCollaboration_<std::vector<TrackerUser>>(
        "FetchIssueWatchers", issueKey, [&issueKey](ITrackerCollaboration& collab) {
            return collab.FetchIssueWatchers(ConfigManager::Load(), issueKey);
        });
}

Result<TrackerIssueVotes> AppController::FetchIssueVotes(const std::string& issueKey) const {
    return smatchet::collab::CollaborationResultToResult<TrackerIssueVotes>(FetchIssueVotesTyped(issueKey));
}

Result<TrackerIssueVotes, TrackerError> AppController::FetchIssueVotesTyped(const std::string& issueKey) const {
    return callFocusedCollaboration_<TrackerIssueVotes>(
        "FetchIssueVotes", issueKey,
        [&issueKey](ITrackerCollaboration& collab) { return collab.FetchIssueVotes(ConfigManager::Load(), issueKey); });
}

Result<std::vector<TrackerUser>> AppController::SearchUsersByQuery(const std::string& query) const {
    return smatchet::collab::CollaborationResultToResult<std::vector<TrackerUser>>(SearchUsersByQueryTyped(query));
}

Result<std::vector<TrackerUser>, TrackerError> AppController::SearchUsersByQueryTyped(const std::string& query) const {
    return callFocusedCollaboration_<std::vector<TrackerUser>>(
        "SearchUsersByQuery", TruncateForLog(query, 120),
        [&query](ITrackerCollaboration& collab) { return collab.SearchUsersByQuery(ConfigManager::Load(), query); });
}

Result<std::vector<TrackerUser>>
AppController::FetchUsersByAccountIds(const std::vector<std::string>& accountIds) const {
    using UsersResult = Result<std::vector<TrackerUser>>;
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (!backend) {
        return UsersResult::Err("Tracker backend is not initialized.");
    }
    if (!backend->Collaboration()) {
        return UsersResult::Err("Tracker backend does not support collaboration features.");
    }
    const TrackerConfig cfg = ConfigManager::Load();
    UsersResult outcome = smatchet::collab::CollaborationResultToResult<std::vector<TrackerUser>>(
        backend->Collaboration()->FetchUsersByAccountIds(cfg, accountIds));
    if (!outcome.has_value()) {
        LOG_ERROR("AppController::FetchUsersByAccountIds failed ids=%zu err=%s", accountIds.size(),
                  outcome.error().c_str());
        return outcome;
    }
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return outcome;
}

Result<std::vector<TrackerIssueComment>> AppController::FetchIssueComments(const std::string& issueKey) {
    return smatchet::collab::CollaborationResultToResult<std::vector<TrackerIssueComment>>(
        FetchIssueCommentsTyped(issueKey));
}

Result<std::vector<TrackerIssueComment>, TrackerError>
AppController::FetchIssueCommentsTyped(const std::string& issueKey) {
    return callFocusedCollaboration_<std::vector<TrackerIssueComment>>(
        "FetchIssueComments", issueKey,
        [&issueKey](ITrackerCollaboration& collab) { return collab.FetchIssueComments(issueKey); });
}

void AppController::UpdateCachedCommentsFromThread(const std::string& issueId,
                                                   const std::vector<TrackerIssueComment>& comments) {
    // issue-comments fix (#1291, extended) — after a comment-thread fetch (comments modal open,
    // post-success re-fetch, or the grid tooltip's first-hover lazy fetch), push the observed
    // thread into the cached ticket so the grid reflects it without a full re-sync: the numeric
    // `comments` count AND the flattened `comment` blob the Comments-cell tooltip renders. The
    // blob is formatted once here (FormatCommentBlob — the same pipeline the Jira search mapper
    // uses), never per frame. Called on the UI thread from a main-thread post-back (mirrors the
    // optimistic-update pattern: mutate a ticket copy → UpdateTicket → SaveTicket + grid refresh).
    // Skips when both values are unchanged, so a routine modal-open fetch causes no grid churn.
    std::shared_ptr<const std::vector<CachedTicket>> snapshot = GetActiveTicketsSnapshot();
    if (!snapshot) {
        return;
    }
    const std::string newCount = std::to_string(comments.size());
    const std::string newBlob = smatchet::tracker::FormatCommentBlob(comments);
    // The structured thread is what the comments modal shows offline (Pillar 6); the blob alone
    // is capped display text.
    const std::string newThread = smatchet::tracker::SerializeCommentThread(comments);
    for (const CachedTicket& ticket : *snapshot) {
        if (ticket.id != issueId) {
            continue;
        }
        const auto threadIt = ticket.fieldRichValues.find(kCommentThreadRichKey);
        const bool threadSame =
            threadIt == ticket.fieldRichValues.end() ? newThread.empty() : threadIt->second == newThread;
        if (ticket.GetFieldValueRef("comments") == newCount && ticket.GetFieldValueRef("comment") == newBlob &&
            threadSame) {
            return; // no change (avoid a needless grid refresh)
        }
        CachedTicket updated = ticket;
        updated.fieldValues["comments"] = newCount;
        updated.fieldValues["comment"] = newBlob;
        if (newThread.empty()) {
            updated.fieldRichValues.erase(kCommentThreadRichKey);
        } else {
            updated.fieldRichValues[kCommentThreadRichKey] = newThread;
        }
        UpdateTicket(updated);
        return;
    }
}

Result<std::vector<std::string>> AppController::FetchUserGroupNames(const std::string& accountId) const {
    using GroupNamesResult = Result<std::vector<std::string>>;
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (!backend) {
        return GroupNamesResult::Err("Jira backend is not initialized.");
    }
    if (!backend->Activity()) {
        return GroupNamesResult::Err("Tracker backend does not support activity features.");
    }
    const TrackerConfig cfg = ConfigManager::Load();
    GroupNamesResult outcome = smatchet::collab::CollaborationResultToResult<std::vector<std::string>>(
        backend->Activity()->FetchUserGroupNames(cfg, accountId));
    if (!outcome.has_value()) {
        LOG_ERROR("AppController::FetchUserGroupNames failed account=%s err=%s", TruncateForLog(accountId, 40).c_str(),
                  outcome.error().c_str());
        return outcome;
    }
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return outcome;
}

bool AppController::PaneSupportsActivity(const std::string& paneId) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &paneContextOrFocused_(paneId)
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    return backend && backend->Activity() != nullptr;
}

Result<std::vector<TrackerActivityEntry>>
AppController::FetchPaneUserActivity(const std::string& paneId, const std::string& accountId,
                                     const std::string& dayFrom, const std::string& dayTo,
                                     const std::string& projectScope, TrackerActivityProgress& progress) const {
    using ActivityResult = Result<std::vector<TrackerActivityEntry>>;
    // Issue #1457: this runs on the User Info activity std::async worker, so resolve the context
    // under the map mutex (exact-id, fallback to the permanent focused context) and latch the
    // backend shared_ptr inside the critical section. The latch (ADR-0012) is the only thing
    // carried across the blocking fetch; the map mutex is released the moment the pointer is read.
    std::shared_ptr<ITrackerBackend> backend;
    {
        std::lock_guard<std::mutex> mapLk(gridContextsMutex_);
        std::map<std::string, std::unique_ptr<GridLiveContext>>::const_iterator it = gridContexts_.find(paneId);
        const GridLiveContext* ctx = (it != gridContexts_.end()) ? it->second.get() : focusedContextPtr_.load();
        backend = std::atomic_load(&ctx->Backend);
    }
    if (!backend) {
        return ActivityResult::Err("Tracker backend is not initialized.");
    }
    if (!backend->Activity()) {
        return ActivityResult::Err("Tracker backend does not support activity features.");
    }
    const TrackerConfig cfg = ConfigManager::Load();
    // progress stays a reference param — the worker streams live progress into it during the
    // multi-second fetch; only the entries payload + error fold into the Result.
    ActivityResult outcome = smatchet::collab::CollaborationResultToResult<std::vector<TrackerActivityEntry>>(
        backend->Activity()->FetchUserActivity(cfg, accountId, dayFrom, dayTo, projectScope, progress));
    if (!outcome.has_value()) {
        LOG_ERROR("AppController::FetchPaneUserActivity failed pane=%s account=%s err=%s", paneId.c_str(),
                  TruncateForLog(accountId, 40).c_str(), outcome.error().c_str());
        return outcome;
    }
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return outcome;
}

void AppController::ClearPaneUserActivity(const std::string& paneId) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &paneContextOrFocused_(paneId)
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (backend && backend->Activity()) {
        backend->Activity()->ClearUserActivity();
    }
}

Result<std::vector<TrackerUser>> AppController::FetchPaneGroupMembers(const std::string& paneId,
                                                                      const std::string& groupName) {
    using MembersResult = Result<std::vector<TrackerUser>>;
    std::shared_ptr<ITrackerBackend> backend;
    {
        // Issue #1457: this runs on a User Info std::async worker, so snapshot the context pointer
        // under the map mutex (the worker MUST NOT traverse gridContexts_ unguarded while the UI
        // thread retire-erases / emplaces). Resolve exact-id, falling back to the permanent focused
        // context, mirroring paneContextOrFocused_. Release the map mutex BEFORE the per-context
        // roster mutex so the worker never holds both. The husk stays alive (ADR-0012 graveyard)
        // even if retired mid-flight, so the latched pointer cannot dangle.
        GridLiveContext* ctx = nullptr;
        {
            std::lock_guard<std::mutex> mapLk(gridContextsMutex_);
            std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
            ctx = (it != gridContexts_.end()) ? it->second.get() : focusedContextPtr_.load();
        }
        {
            std::lock_guard<std::mutex> lock(ctx->groupRoster.rosterMutex_);
            std::unordered_map<std::string, std::vector<TrackerUser>>::const_iterator hit =
                ctx->groupRoster.MembersByGroup.find(groupName);
            if (hit != ctx->groupRoster.MembersByGroup.end()) {
                return MembersResult::Ok(
                    hit->second); // cache hit — no HTTP, no success-notify (matches prior early-return)
            }
        }
        backend = std::atomic_load(&ctx->Backend);
    }
    if (!backend) {
        return MembersResult::Err("Tracker backend is not initialized.");
    }
    if (!backend->Activity()) {
        return MembersResult::Err("Tracker backend does not support activity features.");
    }
    const TrackerConfig cfg = ConfigManager::Load();
    MembersResult outcome = smatchet::collab::CollaborationResultToResult<std::vector<TrackerUser>>(
        backend->Activity()->FetchGroupMembers(cfg, groupName));
    if (outcome.has_value()) {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    } else {
        LOG_ERROR("AppController::FetchPaneGroupMembers failed pane=%s group=%s err=%s", paneId.c_str(),
                  TruncateForLog(groupName, 40).c_str(), outcome.error().c_str());
    }
    // Re-resolve for the write-back — the context may have been retired during the fetch.
    // Exact-id only: caching a fallback pane's roster into the focused context would mix
    // backends (the per-context invariant GridContextGroupRoster exists to keep).
    // Issue #1457: snapshot the pointer under the map mutex (worker thread), then release it
    // BEFORE the per-context roster mutex so the worker never holds map-mutex + roster-mutex
    // together. Writing into a retired husk's roster is a harmless no-op (nobody reads it).
    GridLiveContext* ctx = nullptr;
    {
        std::lock_guard<std::mutex> mapLk(gridContextsMutex_);
        std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
        ctx = (it != gridContexts_.end()) ? it->second.get() : nullptr;
    }
    if (ctx) {
        std::lock_guard<std::mutex> lock(ctx->groupRoster.rosterMutex_);
        if (outcome.has_value()) {
            ctx->groupRoster.MembersByGroup[groupName] = outcome.value();
            ctx->groupRoster.LastGroupRosterError.clear();
        } else {
            ctx->groupRoster.LastGroupRosterError = outcome.error();
        }
    }
    return outcome;
}
