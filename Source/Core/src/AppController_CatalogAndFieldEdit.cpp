#include "AppController.h"
#include "EditMetaCacheService.h"     // editmeta delegators forward to editMeta_ (god-object decomposition Phase 1).
#include "FieldEditPipelineService.h" // field-edit delegators forward to fieldEdit_ (decomposition Phase 2).
#include "ITrackerIssueMutations.h" // fan-in Phase 2: AppController.h fwd-decls it now; this TU calls Mutations() methods.
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls Cache-> methods.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "BackendAuditTrail.h"
#include "FieldEditAuditSource.h"
#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "ITrackerActivity.h"
#include "JiraClient.h"
#include "ProjectResolver.h"
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

} // namespace

void AppController::RefreshLocalData() { RefreshLocalDataCheckedImpl_(focusedContext(), nullptr); }

void AppController::RefreshLocalDataCheckedImpl_(GridLiveContext& ctx, const std::uint64_t* capturedBackendGeneration) {
    if (Cache) {
        // Full-table read stays OUTSIDE activeTicketsMutex_ (SQLite I/O under the tickets
        // mutex would block the UI-thread readers, Pillar 2); the generation re-check below
        // is therefore the authoritative gate, taken immediately before the swap-in.
        const std::string cacheKey = ctx.CacheBackendKeyCopy();
        auto latestTickets = Cache->GetAllTickets(cacheKey);
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
        PruneEditMetaCacheToActiveTickets();
        ctx.ActiveTicketsRevision.fetch_add(1);
        // Single hook site for Lua window dirty-bump on ticket data change. UpdateTicket
        // already chains here through `RefreshLocalData()` at the end of its body, so a
        // duplicate bump from UpdateTicket would double-fire per single edit. Stub build
        // turns this into a no-op.
        NotifyLuaTicketDataChanged();
    }
}

void AppController::RefreshLocalDataAndWarmIssueTypeMeta() {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    RefreshLocalData();
    if (backend) {
        WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());
    }
}

void AppController::UpdateTicket(const CachedTicket& ticket) {
    if (Cache) {
        // Latch key + generation TOGETHER (issue #1081): reading the key here and re-reading
        // it inside RefreshLocalData raced a backend swap — the save landed under the OLD key
        // while the refresh read the NEW key (proven TOCTOU: a Jira row contaminating the
        // GitHub namespace). The generation re-load only detects a swap between the two latch
        // loads — it does NOT close the window before SaveTicket. A swap landing after the
        // check is benign: the write still lands under the CAPTURED key, which is exactly
        // where the row belongs; the checked refresh below then drops the stale grid replace.
        GridLiveContext& ctx = focusedContext();
        const std::uint64_t capturedGeneration = ctx.backendGeneration_.load();
        const std::string capturedKey = ctx.CacheBackendKeyCopy();
        if (ctx.backendGeneration_.load() != capturedGeneration) {
            // WARN, not INFO: callers reach UpdateTicket after the backend mutation already
            // succeeded upstream — dropping the local save leaves a stale row until the old
            // backend's next sync.
            LOG_WARN("AppController::UpdateTicket skipped — backend swapped during key latch (issue #1081): "
                     "key='%s' ticket='%s' generation=%llu",
                     capturedKey.c_str(), ticket.id.c_str(), static_cast<unsigned long long>(capturedGeneration));
            return;
        }
        Cache->SaveTicket(capturedKey, ticket);
        // Push changes back to ActiveTickets — checked against the SAME latched ctx (MEDIUM-1).
        RefreshLocalDataCheckedImpl_(ctx, &capturedGeneration);
    }
}

bool AppController::RefreshFieldCatalog(const TrackerConfig& cfg) { return RefreshFieldCatalog(cfg, std::string()); }

bool AppController::RefreshFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey) {
    // Latch a strong handle for the duration of this call. RefreshFieldCatalog runs on
    // a background worker thread — the new-issue draft / picker catalog refresh — and a live tracker switch
    // (SetBackend on the UI thread) would otherwise free `Backend` mid-FetchFieldCatalog — the
    // FieldCatalog object dereferenced below lives inside it. The shared_ptr copy keeps the old
    // backend alive until this call returns, even after app_.Backend is swapped (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call, so a
    // focus switch between two calls inside one locked/compound region would lock context A's
    // mutex while mutating context B (UB, Pillar 3). The retired-context husk graveyard keeps
    // the latched reference valid for the life of this call.
    GridContextFieldCatalog& cat = fieldCatalog();
    if (!backend) {
        {
            std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
            cat.currentCatalogProjectKey_ = projectKey;
        }
        SetFieldCatalog({}, {}, "Tracker backend is not initialized."); // config-class: non-transient default
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        cat.currentCatalogProjectKey_ = projectKey;
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
        SetFieldCatalog({}, {}, error, errorTransient);
        LOG_ERROR("AppController::RefreshFieldCatalog failed: %s", error.c_str());
        return false;
    }

    SetFieldCatalog(std::move(catalog.Fields), std::move(catalog.Components), std::move(catalog.IssueTypeMeta), {});
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

void AppController::SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                                    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error,
                                    bool errorTransient) {
    const TrackerConfig cfgSnap = ConfigManager::Load();
    const bool catalogPlane = ConfigManager::NormalizeViewsBackendKey(cfgSnap.TrackerType) == "Plane";
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would lock context A's mutex while mutating context B (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
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
        HandleFieldCatalogError(error, errorTransient, catalogCacheKey, catalogPlane);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        cat.AvailableFields = std::move(fields);
        cat.AvailableComponents = std::move(components);
        cat.AvailableIssueTypeMeta = std::move(issueTypeMeta);
    }
    cat.LastTrackerFieldCatalogError.clear();
    cat.LastTrackerFieldCatalogErrorTransient = false;
    cat.LastTrackerFieldCatalogWarning.clear();
    cat.fieldCatalogEverLoaded_ = true;
    requestDeferredLiveTrackerBackendSuccessNotify_();
    {
        std::string snapErr;
        const std::string saveBackend = catalogPlane ? std::string("Plane") : std::string("Jira");
        const std::string saveEndpoint =
            catalogPlane ? (cfgSnap.PlaneUrl + std::string("|") + cfgSnap.PlaneWorkspaceSlug) : cfgSnap.Domain;
        if (!FieldCatalogCache::SaveFieldCatalogSnapshot(
                catalogCacheKey, saveBackend, saveEndpoint, projectKeyForCache, cfgSnap.FieldCatalogCacheMaxProjects,
                cat.AvailableFields, cat.AvailableComponents, cat.AvailableIssueTypeMeta, snapErr)) {
            LOG_WARN("AppController::SetFieldCatalog: snapshot save failed: %s", snapErr.c_str());
        }
    }
    if (!catalogPlane) {
        // DR6: the timetracking read-only sweep mutates cat.AvailableFields, which UI-thread
        // readers (create/draft paths) touch concurrently — take the guard for the loop. The
        // Erase/Ensure helpers below self-lock availableFieldsMutex_ (non-recursive), so they
        // must stay OUTSIDE this scope or the second lock self-deadlocks.
        {
            std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
            for (auto& field : cat.AvailableFields) {
                if (IsNonEditableTimetrackingFieldId(field.Id)) {
                    field.ReadOnly = true;
                }
            }
        }
        EraseCatalogLegacyCommentField(cat);
        EnsureCatalogHistoryField(cat);
        EnsureCatalogCommentsField(cat);
    }

    cat.TrackerFieldCatalogRevision.fetch_add(1);
}

void AppController::HandleFieldCatalogError(const std::string& error, bool errorTransient,
                                            const std::string& catalogCacheKey, bool catalogPlane) {
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would lock context A's mutex while mutating context B (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    // N12 item 13b: the flag was classified where the catalog fetch's TrackerError was
    // flattened — no re-classification of the text here.
    if (!errorTransient) {
        {
            std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
            cat.AvailableFields.clear();
            cat.AvailableComponents.clear();
            cat.AvailableIssueTypeMeta.clear();
        }
        cat.fieldCatalogEverLoaded_ = false;
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogError = error;
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.TrackerFieldCatalogRevision.fetch_add(1);
        LOG_ERROR("AppController::SetFieldCatalog error: %s", error.c_str());
        return;
    }

    bool catalogHasFields;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        catalogHasFields = !cat.AvailableFields.empty();
    }
    if (catalogHasFields) {
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        const std::string nextWarning = std::string("Offline: using cached ") + (catalogPlane ? "Plane" : "Jira") +
                                        " field catalog. Last fetch failed: " + error;
        if (nextWarning != cat.LastTrackerFieldCatalogWarning) {
            cat.LastTrackerFieldCatalogWarning = nextWarning;
            cat.TrackerFieldCatalogRevision.fetch_add(1);
        }
        LOG_WARN("AppController::SetFieldCatalog transport failure (catalog preserved): %s", error.c_str());
        return;
    }

    std::vector<TrackerField> snapFields;
    std::vector<TrackerComponent> snapComponents;
    std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta;
    std::string snapErr;
    if (FieldCatalogCache::TryLoadFieldCatalogSnapshot(catalogCacheKey, snapFields, snapComponents, snapIssueTypeMeta,
                                                       snapErr)) {
        {
            std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
            cat.AvailableFields = std::move(snapFields);
            cat.AvailableComponents = std::move(snapComponents);
            cat.AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
            if (!catalogPlane) {
                for (auto& field : cat.AvailableFields) {
                    if (IsNonEditableTimetrackingFieldId(field.Id)) {
                        field.ReadOnly = true;
                    }
                }
            }
        }
        cat.fieldCatalogEverLoaded_ = true;
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning = std::string("Offline: restored ") + (catalogPlane ? "Plane" : "Jira") +
                                             " field catalog from local snapshot. Last fetch failed: " + error;
        if (!catalogPlane) {
            EraseCatalogLegacyCommentField(cat);
            EnsureCatalogHistoryField(cat);
            EnsureCatalogCommentsField(cat);
        }
        cat.TrackerFieldCatalogRevision.fetch_add(1);
        LOG_WARN("AppController::SetFieldCatalog transport failure; loaded snapshot err=%s", snapErr.c_str());
        return;
    }

    if (cat.fieldCatalogEverLoaded_) {
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning =
            "Offline: no field catalog snapshot could be loaded for this tracker context. Last fetch failed: " + error;
        cat.TrackerFieldCatalogRevision.fetch_add(1);
        LOG_WARN("AppController::SetFieldCatalog transport failure; no snapshot (session had catalog): %s",
                 error.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        cat.AvailableFields.clear();
        cat.AvailableComponents.clear();
        cat.AvailableIssueTypeMeta.clear();
    }
    cat.fieldCatalogEverLoaded_ = false;
    cat.LastTrackerFieldCatalogWarning.clear();
    cat.LastTrackerFieldCatalogErrorTransient = true; // this branch is reached only on a transport-shaped failure
    cat.LastTrackerFieldCatalogError = std::string("No cached ") + (catalogPlane ? "Plane" : "Jira") +
                                       " field catalog available. " +
                                       (error.empty() ? std::string("Last fetch failed.") : error);
    cat.TrackerFieldCatalogRevision.fetch_add(1);
    LOG_ERROR("AppController::SetFieldCatalog error (no cache): %s", error.c_str());
}

const TrackerField* AppController::FindFieldById(const std::string& fieldId) const {
    const GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    const auto it = std::find_if(cat.AvailableFields.begin(), cat.AvailableFields.end(),
                                 [&](const TrackerField& field) { return field.Id == fieldId; });
    return it == cat.AvailableFields.end() ? nullptr : &(*it);
}

void AppController::EnsureCatalogHistoryField(GridContextFieldCatalog& cat) {
    // Atomic check-then-insert under the catalog lock (#823). The existence
    // check is done INLINE (not via FindFieldById, which locks the same
    // non-recursive availableFieldsMutex_ → would self-deadlock) so the lookup
    // and the push_back can't race a concurrent catalog read/write. `cat` is the
    // caller's latched catalog — must NOT re-resolve fieldCatalog() here, or a focus
    // switch could insert into a different context than the caller populated.
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    const auto it = std::find_if(cat.AvailableFields.begin(), cat.AvailableFields.end(),
                                 [](const TrackerField& field) { return field.Id == "history"; });
    if (it != cat.AvailableFields.end()) {
        return;
    }
    TrackerField historyField;
    historyField.Id = "history";
    historyField.Name = "History";
    historyField.ReadOnly = true;
    cat.AvailableFields.push_back(std::move(historyField));
}

void AppController::EnsureCatalogCommentsField(GridContextFieldCatalog& cat) {
    // issue-comments PR-B — synthetic read-only `comments` count column for Jira. Mirrors the
    // sibling history-field helper above: same atomic check-then-insert under the catalog lock,
    // INLINE find_if rather than FindFieldById to avoid self-deadlocking the non-recursive mutex.
    // Type "number" matches the GitHub catalog's comments field so the shared comments cell
    // special-case renders a count. Read-only via ReadOnly=true alone — no Jira editmeta entry
    // needed (the sibling `history` synthetic field is the precedent). `cat` is the caller's
    // latched catalog — do NOT re-resolve fieldCatalog() here (see header).
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    const auto it = std::find_if(cat.AvailableFields.begin(), cat.AvailableFields.end(),
                                 [](const TrackerField& field) { return field.Id == "comments"; });
    if (it != cat.AvailableFields.end()) {
        return;
    }
    TrackerField commentsField;
    commentsField.Id = "comments";
    commentsField.Name = "Comments";
    commentsField.Type = "number";
    commentsField.ReadOnly = true;
    cat.AvailableFields.push_back(std::move(commentsField));
}

void AppController::EraseCatalogLegacyCommentField(GridContextFieldCatalog& cat) {
    // issue-comments fix (#1291 follow-up) — drop Jira's legacy system `comment` field (ADF blob,
    // catalog label "Comment") from the picker. It duplicates the synthetic `comments` count column
    // (EnsureCatalogCommentsField): the user saw two "Comment(s)" entries. The blob still rides in
    // per-ticket fieldValues["comment"] via the Jira mapper (catalog-independent) and surfaces as the
    // Comments-cell hover tooltip — so dropping it from the catalog removes the duplicate picker entry
    // without losing the text. Mirrors the sibling Ensure* helpers: own the catalog lock, INLINE scan
    // (not FindFieldById, which re-locks the non-recursive mutex → self-deadlock). `cat` is the
    // caller's latched catalog — do NOT re-resolve fieldCatalog() here (see header).
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    cat.AvailableFields.erase(std::remove_if(cat.AvailableFields.begin(), cat.AvailableFields.end(),
                                             [](const TrackerField& field) { return field.Id == "comment"; }),
                              cat.AvailableFields.end());
}

// FieldEditSupportsOfflineQueue + the field-edit network pipeline (TryBuildFieldEditPayloadForNetwork,
// SubmitFieldEdit[Sprint/Timetracking/Regular], SubmitFieldEdit, SubmitFieldEditNetworkOnly + its
// helpers, TryPrepareOfflineFieldEdit, ApplyFieldEditResult) now live in FieldEditPipelineService
// (god-object decomposition Phase 2). Only thin public delegators remain here; see the delegator
// block below. The editmeta cache methods moved earlier (Phase 1, EditMetaCacheService).

bool AppController::FieldEditSupportsOfflineQueue(const TrackerField& field) {
    return FieldEditPipelineService::FieldEditSupportsOfflineQueue(field);
}

std::vector<TrackerFieldOption> AppController::GetComponentOptionsForProject(const std::string& projectKey) const {
    const GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    const auto it = cat.projectComponentOptions_.find(projectKey);
    if (it == cat.projectComponentOptions_.end()) {
        return std::vector<TrackerFieldOption>();
    }
    return it->second;
}

bool AppController::IsProjectComponentsLoaded(const std::string& projectKey) const {
    const GridContextFieldCatalog& cat =
        fieldCatalog(); // latch once — lock/object must resolve to the same context (Pillar 3)
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    return cat.projectComponentOptions_.find(projectKey) != cat.projectComponentOptions_.end();
}

void AppController::EnsureProjectComponentsLoaded(const std::string& projectKey) {
    if (projectKey.empty()) {
        return;
    }
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (!backend) {
        return;
    }
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would lock context A's mutex while mutating context B (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    {
        std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
        if (cat.projectComponentOptions_.find(projectKey) != cat.projectComponentOptions_.end()) {
            return; // already warmed
        }
        if (!cat.projectComponentsInFlight_.insert(projectKey).second) {
            return; // a fetch for this project is already running
        }
        // Failure backoff: a previous fetch failed and recorded a retry deadline. Skip relaunch
        // (and undo the in-flight marker we just took) until that deadline passes — otherwise the
        // every-frame paint-path call would hammer the backend after each failure.
        const auto retryIt = cat.projectComponentsRetryAfter_.find(projectKey);
        if (retryIt != cat.projectComponentsRetryAfter_.end() && std::chrono::steady_clock::now() < retryIt->second) {
            cat.projectComponentsInFlight_.erase(projectKey);
            return;
        }
    }
    TrackerConfig trackerCfgForWorker = ConfigManager::Load();
    // Capture the KICK-TIME catalog by pointer (#975). fieldCatalog() re-resolves
    // focusedContextPtr_ at CALL time; re-resolving it inside the worker would target whatever
    // context is focused at COMPLETION time, not the one whose projectComponentsInFlight_ marker
    // we just inserted above. A focus switch mid-fetch would then leave THIS context's marker set
    // forever (the pane stuck "Loading components…" until restart) while spuriously
    // erasing/writing the completion-time context. Capturing &catPtr pins the kick-time context's
    // catalog for the whole worker run.
    // Dangle-safety: a GridContextFieldCatalog is a subobject of a GridLiveContext. The UI thread
    // never frees a live context mid-session — on retirement it moves the context (as a defer-free
    // husk) into retiredContexts_, which survives until ~AppController (ADR-0012 graveyard applied
    // to contexts; see AppController.h retiredContexts_). The husk's availableFieldsMutex_ and
    // containers stay valid (cleared, not destroyed), so this raw pointer is always safe to lock
    // and mutate. If the context was retired mid-fetch, erasing the marker on the husk is a no-op
    // that leaks nothing visible (a husk is never painted), so no extra guard is needed.
    GridContextFieldCatalog* catPtr = &cat;
    LaunchBackgroundTask([this, projectKey, backend, catPtr,
                          trackerCfgForWorker = std::move(trackerCfgForWorker)]() mutable {
        // Operate on the kick-time context (#975) — NOT a completion-time fieldCatalog() re-resolve.
        GridContextFieldCatalog& cat = *catPtr;
        if (shuttingDown_.load()) {
            std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
            cat.projectComponentsInFlight_.erase(projectKey);
            return;
        }
        ITrackerFieldCatalog* catalog = backend ? backend->FieldCatalog() : nullptr;
        if (catalog == nullptr) {
            std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
            cat.projectComponentsInFlight_.erase(projectKey);
            return;
        }
        // Lock is NOT held across the HTTP call — fetch into locals, then lock-insert.
        auto componentsResult = catalog->FetchProjectComponents(trackerCfgForWorker, projectKey);
        if (!componentsResult) {
            LOG_DEBUG("AppController: lazy per-project component load failed for %s: %s", projectKey.c_str(),
                      componentsResult.error().Detail.c_str());
            std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
            // Record a backoff deadline so the every-frame paint path stops relaunching until it
            // passes (~once / 30s per failing project instead of every frame).
            cat.projectComponentsRetryAfter_[projectKey] = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            cat.projectComponentsInFlight_.erase(projectKey);
            return;
        }
        std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
        // Insert the key on success regardless of count — presence == loaded, so a genuinely empty
        // project settles to "(no options)" instead of showing "Loading components…" forever.
        cat.projectComponentOptions_[projectKey] = std::move(componentsResult.value().Options);
        cat.projectComponentsRetryAfter_.erase(projectKey); // success clears any prior backoff
        cat.projectComponentsInFlight_.erase(projectKey);
    });
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
    editMeta_->WarmIssueTypeEditMetaAtStartAsync(std::move(trackerCfgForWorker));
}

void AppController::WarmIssueEditMetaAsync(const std::string& issueId) { editMeta_->WarmIssueEditMetaAsync(issueId); }

// Field-edit pipeline delegators — forward to `fieldEdit_` (FieldEditPipelineService, god-object
// decomposition Phase 2). The service owns the SubmitFieldEditCtx struct + the branch helpers +
// TryBuildFieldEditPayloadForNetwork (all service-private now). `fieldEdit_` is constructed eagerly
// in Initialize, so it is non-null for every call after startup.

VoidResult AppController::SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                          const std::vector<std::string>& rawValues) {
    return fieldEdit_->SubmitFieldEdit(issueId, field, rawValues);
}

FieldEditResult AppController::SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                                          const std::vector<std::string>& rawValues,
                                                          const std::string& originalEstimateSnapshot,
                                                          const std::string& remainingEstimateSnapshot,
                                                          const std::string& issueTypeKeySnapshot) {
    return fieldEdit_->SubmitFieldEditNetworkOnly(issueId, field, rawValues, originalEstimateSnapshot,
                                                  remainingEstimateSnapshot, issueTypeKeySnapshot);
}

bool AppController::TryPrepareOfflineFieldEdit(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues,
                                               const std::string& originalEstimateSnapshot,
                                               const std::string& remainingEstimateSnapshot,
                                               const std::string& issueTypeKeySnapshot, FieldEditResult& outResult,
                                               std::string& outFieldsPayloadJson, std::string& outError) {
    return fieldEdit_->TryPrepareOfflineFieldEdit(issueId, field, rawValues, originalEstimateSnapshot,
                                                  remainingEstimateSnapshot, issueTypeKeySnapshot, outResult,
                                                  outFieldsPayloadJson, outError);
}

VoidResult AppController::ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result) {
    return fieldEdit_->ApplyFieldEditResult(issueId, result);
}
bool AppController::FetchIssueWatchers(const std::string& issueKey, std::vector<TrackerUser>& outWatchers,
                                       std::string& outError) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outWatchers.clear();
    outError.clear();
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    auto watchersResult = backend->Collaboration()->FetchIssueWatchers(cfg, issueKey);
    const bool ok = static_cast<bool>(watchersResult);
    if (ok) {
        outWatchers = std::move(watchersResult.value());
    } else {
        outError = watchersResult.error().Detail;
    }
    if (!ok) {
        LOG_ERROR("AppController::FetchIssueWatchers failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::AddIssueWatcher(const std::string& issueKey, std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::AddIssueWatcher blocked by read-only mode issue=%s", issueKey.c_str());
        return false;
    }
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const TrackerError addWatcherErr = backend->Collaboration()->AddIssueWatcher(cfg, issueKey);
    const bool ok = addWatcherErr.IsOk();
    if (!ok) {
        outError = addWatcherErr.Detail;
        LOG_ERROR("AppController::AddIssueWatcher failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::FetchIssueVotes(const std::string& issueKey, std::vector<TrackerUser>& outVoters,
                                    std::string& outError, int* outVoteCount, bool* outHasVoted,
                                    bool* outVotersInResponse) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outVoters.clear();
    outError.clear();
    if (outVoteCount) {
        *outVoteCount = 0;
    }
    if (outHasVoted) {
        *outHasVoted = false;
    }
    if (outVotersInResponse) {
        *outVotersInResponse = false;
    }
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    auto votesResult = backend->Collaboration()->FetchIssueVotes(cfg, issueKey);
    const bool ok = static_cast<bool>(votesResult);
    if (ok) {
        const TrackerIssueVotes& votes = votesResult.value();
        outVoters = votes.Voters;
        if (outVoteCount) {
            *outVoteCount = votes.VoteCount;
        }
        if (outHasVoted) {
            *outHasVoted = votes.HasVoted;
        }
        if (outVotersInResponse) {
            *outVotersInResponse = votes.VotersArrayInResponse;
        }
    } else {
        outError = votesResult.error().Detail;
    }
    if (!ok) {
        LOG_ERROR("AppController::FetchIssueVotes failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::SearchUsersByQuery(const std::string& query, std::vector<TrackerUser>& outUsers,
                                       std::string& outError) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outUsers.clear();
    outError.clear();
    if (!backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    auto usersResult = backend->Collaboration()->SearchUsersByQuery(cfg, query);
    const bool ok = static_cast<bool>(usersResult);
    if (ok) {
        outUsers = std::move(usersResult.value());
    } else {
        outError = usersResult.error().Detail;
    }
    if (!ok) {
        LOG_ERROR("AppController::SearchUsersByQuery failed query=%s err=%s", TruncateForLog(query, 120).c_str(),
                  outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::AddIssueCommentPlain(const std::string& issueKey, const std::string& plainText,
                                         std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::AddIssueCommentPlain blocked by read-only mode issue=%s", issueKey.c_str());
        return false;
    }
    if (!backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const TrackerError commentErr = backend->Collaboration()->AddIssueCommentPlain(cfg, issueKey, plainText);
    const bool ok = commentErr.IsOk();
    if (!ok) {
        outError = commentErr.Detail;
        LOG_ERROR("AppController::AddIssueCommentPlain failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::FetchIssueComments(const std::string& issueKey, std::vector<TrackerIssueComment>& outComments,
                                       std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outComments.clear();
    outError.clear();
    if (!backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    auto r = backend->Collaboration()->FetchIssueComments(issueKey);
    if (r) {
        outComments = std::move(r.value());
        requestDeferredLiveTrackerBackendSuccessNotify_();
        return true;
    }
    outError = r.error().Detail;
    LOG_ERROR("AppController::FetchIssueComments failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    return false;
}

void AppController::UpdateCachedCommentCount(const std::string& issueId, int newCount) {
    // issue-comments fix (#1291) — after the comments modal fetches / re-fetches a thread, push the live
    // count into the cached ticket so the grid's Comments column reflects a freshly-posted comment
    // without a full re-sync. Called on the UI thread from the modal's main-thread post-back (mirrors
    // the optimistic-update pattern: mutate a ticket copy → UpdateTicket → SaveTicket + grid refresh).
    // Two gates keep it well-behaved: (1) only count-backed backends — if the cell currently carries no
    // count (Plane leaves fieldValues["comments"] empty for its icon-only cell), leave it empty so the
    // cell stays icon-only; (2) skip when unchanged, so the routine modal-open fetch causes no churn.
    if (newCount < 0) {
        return;
    }
    std::shared_ptr<const std::vector<CachedTicket>> snapshot = GetActiveTicketsSnapshot();
    if (!snapshot) {
        return;
    }
    const std::string newValue = std::to_string(newCount);
    for (const CachedTicket& ticket : *snapshot) {
        if (ticket.id != issueId) {
            continue;
        }
        const std::string& existing = ticket.GetFieldValueRef("comments");
        if (existing.empty() || existing == newValue) {
            return; // icon-only backend (keep empty) or no change (avoid a needless grid refresh)
        }
        CachedTicket updated = ticket;
        updated.fieldValues["comments"] = newValue;
        UpdateTicket(updated);
        return;
    }
}

bool AppController::SubmitWorklog(const std::string& issueId, const std::string& timeSpent,
                                  const std::string& timeRemaining, const std::string& adjustEstimate,
                                  const std::string& workDescription, const std::string& startedDate,
                                  std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::SubmitWorklog blocked by read-only mode issue=%s", issueId.c_str());
        return false;
    }
    if (!backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const TrackerError worklogErr = backend->Collaboration()->AddWorklog(cfg, issueId, timeSpent, timeRemaining,
                                                                         adjustEstimate, workDescription, startedDate);
    const bool ok = worklogErr.IsOk();
    if (!ok) {
        outError = worklogErr.Detail;
        LOG_ERROR("AppController::SubmitWorklog failed issue=%s err=%s", issueId.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
        PrefetchIssueTicketsForKeys({issueId}, true);
    }
    return ok;
}

bool AppController::AddIssueCommentAnnotateContext(const std::string& issueKey, const std::string& p4User,
                                                   const std::string& functionName, const std::string& filePath,
                                                   const int lineNumber, const std::string& changelist,
                                                   const std::string& date, const bool approximated,
                                                   const std::string& codeSnippet, std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::AddIssueCommentAnnotateContext blocked by read-only mode issue=%s", issueKey.c_str());
        return false;
    }
    if (!backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    if (!backend->Collaboration()) {
        outError = "Tracker backend does not support collaboration features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const TrackerError annotateErr = backend->Collaboration()->AddIssueCommentAnnotateContext(
        cfg, issueKey, p4User, functionName, filePath, lineNumber, changelist, date, approximated, codeSnippet);
    const bool ok = annotateErr.IsOk();
    if (!ok) {
        outError = annotateErr.Detail;
        LOG_ERROR("AppController::AddIssueCommentAnnotateContext failed issue=%s err=%s", issueKey.c_str(),
                  outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::FetchUserGroupNames(const std::string& accountId, std::vector<std::string>& outGroupNames,
                                        std::string& outError) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outGroupNames.clear();
    outError.clear();
    if (!backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    if (!backend->Activity()) {
        outError = "Tracker backend does not support activity features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    auto groupsResult = backend->Activity()->FetchUserGroupNames(cfg, accountId);
    const bool ok = static_cast<bool>(groupsResult);
    if (ok) {
        outGroupNames = std::move(groupsResult.value());
    } else {
        outError = groupsResult.error().Detail;
    }
    if (!ok) {
        LOG_ERROR("AppController::FetchUserGroupNames failed account=%s err=%s", TruncateForLog(accountId, 40).c_str(),
                  outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::PaneSupportsActivity(const std::string& paneId) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &paneContextOrFocused_(paneId)
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    return backend && backend->Activity() != nullptr;
}

bool AppController::FetchPaneUserActivity(const std::string& paneId, const std::string& accountId,
                                          const std::string& dayFrom, const std::string& dayTo,
                                          const std::string& projectScope, TrackerActivityProgress& progress,
                                          std::vector<TrackerActivityEntry>& outEntries, std::string& outError) const {
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
    outEntries.clear();
    outError.clear();
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (!backend->Activity()) {
        outError = "Tracker backend does not support activity features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    auto activityResult =
        backend->Activity()->FetchUserActivity(cfg, accountId, dayFrom, dayTo, projectScope, progress);
    const bool ok = static_cast<bool>(activityResult);
    if (ok) {
        outEntries = std::move(activityResult.value());
        requestDeferredLiveTrackerBackendSuccessNotify_();
    } else {
        outError = activityResult.error().Detail;
        LOG_ERROR("AppController::FetchPaneUserActivity failed pane=%s account=%s err=%s", paneId.c_str(),
                  TruncateForLog(accountId, 40).c_str(), outError.c_str());
    }
    return ok;
}

void AppController::ClearPaneUserActivity(const std::string& paneId) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &paneContextOrFocused_(paneId)
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (backend && backend->Activity()) {
        backend->Activity()->ClearUserActivity();
    }
}

bool AppController::FetchPaneGroupMembers(const std::string& paneId, const std::string& groupName,
                                          std::vector<TrackerUser>& outMembers, std::string& outError) {
    outMembers.clear();
    outError.clear();
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
                outMembers = hit->second;
                return true;
            }
        }
        backend = std::atomic_load(&ctx->Backend);
    }
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (!backend->Activity()) {
        outError = "Tracker backend does not support activity features.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    auto membersResult = backend->Activity()->FetchGroupMembers(cfg, groupName);
    const bool ok = static_cast<bool>(membersResult);
    if (ok) {
        outMembers = std::move(membersResult.value());
        requestDeferredLiveTrackerBackendSuccessNotify_();
    } else {
        outError = membersResult.error().Detail;
        LOG_ERROR("AppController::FetchPaneGroupMembers failed pane=%s group=%s err=%s", paneId.c_str(),
                  TruncateForLog(groupName, 40).c_str(), outError.c_str());
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
        if (ok) {
            ctx->groupRoster.MembersByGroup[groupName] = outMembers;
            ctx->groupRoster.LastGroupRosterError.clear();
        } else {
            ctx->groupRoster.LastGroupRosterError = outError;
        }
    }
    return ok;
}
