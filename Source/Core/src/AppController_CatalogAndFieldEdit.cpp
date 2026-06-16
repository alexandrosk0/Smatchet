#include "AppController.h"
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

bool IsSprintField(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::Sprint || field.SchemaCustom.find("gh-sprint") != std::string::npos;
}

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsEditableTimetrackingEstimateFieldId(fieldId);
}

bool IsNonEditableTimetrackingFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsNonEditableTimetrackingFieldId(fieldId);
}

bool ErrorTextContainsHttpStatus(const std::string& errorText, int statusCode) {
    if (statusCode < 100 || statusCode > 599) {
        return false;
    }
    const std::string needle = "HTTP " + std::to_string(statusCode);
    return errorText.find(needle) != std::string::npos;
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
            // ctx.activeTicketsMutex_ (the SAME context the caller captured from, PR #1104
            // review MEDIUM-1) so the decision is ordered against the swap path's locked
            // clear+publish.
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
            // backend's next sync (PR #1104 review MEDIUM-2).
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
        SetFieldCatalog({}, {}, "Tracker backend is not initialized.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        cat.currentCatalogProjectKey_ = projectKey;
    }
    TrackerFieldCatalogResult catalog;
    std::string error;
    bool ok = false;
    if (backend->FieldCatalog()) {
        auto catalogResult = backend->FieldCatalog()->FetchFieldCatalog(cfg, projectKey);
        ok = static_cast<bool>(catalogResult);
        if (ok) {
            catalog = std::move(catalogResult.value());
        } else {
            error = catalogResult.error().Detail;
        }
    }
    if (!ok) {
        SetFieldCatalog({}, {}, error);
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
    // PR 6: project is per-operation. This convenience overload is called by config-time
    // probes that don't pin a project; backend returns the unscoped catalog.
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
                                    const std::string& error) {
    SetFieldCatalog(std::move(fields), std::move(components), {}, error);
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
                                    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error) {
    const TrackerConfig cfgSnap = ConfigManager::Load();
    const bool catalogPlane = ConfigManager::NormalizeViewsBackendKey(cfgSnap.TrackerType) == "Plane";
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would lock context A's mutex while mutating context B (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    // PR 6: legacy global project fields removed. Saves under the unscoped ("") cache key when
    // the caller hasn't pinned a project via SetCurrentCatalogProject(). Per-project refetches
    // (driven by the new-issue draft / picker UI) set that hint so the snapshot lands under
    // the right per-project entry. PR 7 will replace this with a parameter on the call chain.
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
        HandleFieldCatalogError(error, catalogCacheKey, catalogPlane);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        cat.AvailableFields = std::move(fields);
        cat.AvailableComponents = std::move(components);
        cat.AvailableIssueTypeMeta = std::move(issueTypeMeta);
    }
    cat.LastTrackerFieldCatalogError.clear();
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
        for (auto& field : cat.AvailableFields) {
            if (IsNonEditableTimetrackingFieldId(field.Id)) {
                field.ReadOnly = true;
            }
        }
        EraseCatalogLegacyCommentField(cat);
        EnsureCatalogHistoryField(cat);
        EnsureCatalogCommentsField(cat);
    }

    cat.TrackerFieldCatalogRevision.fetch_add(1);
}

void AppController::HandleFieldCatalogError(const std::string& error, const std::string& catalogCacheKey,
                                            bool catalogPlane) {
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would lock context A's mutex while mutating context B (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    if (!IsTrackerTransportErrorText(error)) {
        {
            std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
            cat.AvailableFields.clear();
            cat.AvailableComponents.clear();
            cat.AvailableIssueTypeMeta.clear();
        }
        cat.fieldCatalogEverLoaded_ = false;
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogError = error;
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
    // switch could insert into a different context than the caller populated (CR PR#1218).
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
    // latched catalog — do NOT re-resolve fieldCatalog() here (CR PR#1218; see header).
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
    // caller's latched catalog — do NOT re-resolve fieldCatalog() here (CR PR#1218; see header).
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    cat.AvailableFields.erase(std::remove_if(cat.AvailableFields.begin(), cat.AvailableFields.end(),
                                             [](const TrackerField& field) { return field.Id == "comment"; }),
                              cat.AvailableFields.end());
}

bool AppController::FieldEditSupportsOfflineQueue(const TrackerField& field) {
    if (IsSprintField(field)) {
        return false;
    }
    if (IsNonEditableTimetrackingFieldId(field.Id) || IsEditableTimetrackingEstimateFieldId(field.Id)) {
        return false;
    }
    switch (field.Family) {
    case TrackerFieldFamily::Text:
    case TrackerFieldFamily::Number:
    case TrackerFieldFamily::Date:
    case TrackerFieldFamily::DateTime:
    case TrackerFieldFamily::Labels:
    case TrackerFieldFamily::SelectSingle:
    case TrackerFieldFamily::SelectMulti:
    case TrackerFieldFamily::UserSingle:
    case TrackerFieldFamily::UserMulti:
    case TrackerFieldFamily::CascadingSelect:
        return true;
    default:
        return false;
    }
}

bool AppController::TryBuildFieldEditPayloadForNetwork(
    const std::string& issueId, const TrackerField& field, const std::vector<std::string>& rawValues,
    const std::string& originalEstimateSnapshot, const std::string& remainingEstimateSnapshot,
    const std::string& issueTypeKeySnapshot, nlohmann::json& outFieldsPayload,
    std::unordered_map<std::string, std::string>& outDisplayValues, std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    (void)originalEstimateSnapshot;
    (void)remainingEstimateSnapshot;
    outError.clear();
    outDisplayValues.clear();
    outFieldsPayload = nlohmann::json::object();
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (IsSprintField(field) || IsNonEditableTimetrackingFieldId(field.Id) ||
        IsEditableTimetrackingEstimateFieldId(field.Id)) {
        outError = "Field type not supported for this edit path.";
        return false;
    }

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    const std::string* issueTypeKeyOpt = issueTypeKeySnapshot.empty() ? nullptr : &issueTypeKeySnapshot;
    if (!IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id)) {
        EnsureIssueEditMetaLoaded(issueId, nullptr, issueTypeKeyOpt);
    }
    if (!IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id) &&
        !CanEditFieldForIssue(issueId, field.Id, &field, issueTypeKeyOpt)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        return false;
    }

    nlohmann::json valuePayload;
    bool built = false;
    if (backend->Mutations()) {
        auto payloadResult = backend->Mutations()->BuildFieldPayload(field, rawValues);
        if (payloadResult) {
            valuePayload = std::move(payloadResult.value());
            built = true;
        } else {
            outError = payloadResult.error().Detail;
        }
    }
    if (!built) {
        LOG_WARN("AppController::TryBuildFieldEditPayloadForNetwork build failed issue=%s field=%s err=%s",
                 issueId.c_str(), field.Id.c_str(), outError.c_str());
        return false;
    }

    outFieldsPayload = std::move(valuePayload);

    std::string displayValue;
    if (!values.empty()) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                displayValue += ", ";
            }
            displayValue += backend->Reader().ResolveDisplayValue(field.Id, &field, values[i]);
        }
    }
    outDisplayValues[field.Id] = std::move(displayValue);
    return true;
}
std::string AppController::ResolveIssueTypeKeyForIssue(const std::string& issueId) const {
    if (issueId.empty()) {
        return std::string();
    }
    const auto ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    const auto it =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (it == tickets.end()) {
        return std::string();
    }
    return ToLowerAsciiCopy(TrimCopy(it->GetFieldValue("issuetype")));
}

void AppController::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (!backend) {
        return;
    }
    const auto ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    std::vector<std::pair<std::string, std::string>> representatives;
    std::unordered_set<std::string> seenTypes;
    seenTypes.reserve(tickets.size());
    // Distinct Jira project keys (issue-key prefixes) across the active tickets — warmed below into
    // fieldCatalog().projectComponentOptions_ so the components MultiSelect editor can scope per row's own project.
    std::vector<std::string> componentProjectKeys;
    std::unordered_set<std::string> seenProjectKeys;
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        for (const auto& ticket : tickets) {
            if (ticket.id.empty()) {
                continue;
            }
            const std::string projectKey = smatchet::ExtractIssueKeyPrefix(ticket.id);
            if (!projectKey.empty() && seenProjectKeys.insert(projectKey).second) {
                componentProjectKeys.push_back(projectKey);
            }
            const std::string typeKey = ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
            if (typeKey.empty()) {
                continue;
            }
            if (seenTypes.find(typeKey) != seenTypes.end()) {
                continue;
            }
            const auto typeIt = issueTypeEditMeta_.find(typeKey);
            if (typeIt != issueTypeEditMeta_.end() && typeIt->second.loaded) {
                seenTypes.insert(typeKey);
                continue;
            }
            seenTypes.insert(typeKey);
            representatives.push_back({typeKey, ticket.id});
        }
    }
    if (representatives.empty() && componentProjectKeys.empty()) {
        return;
    }
    // Capture the KICK-TIME catalog by pointer (#975, sibling of EnsureProjectComponentsLoaded):
    // the worker warms projectComponentsInFlight_ markers (one per project key), so a
    // completion-time fieldCatalog() re-resolve under a focus switch would leak the kick-time
    // context's markers ("Loading components…" forever) and write the completion-time context.
    // Dangle-safety is identical to the lazy path: the catalog is a subobject of a GridLiveContext
    // that the husk graveyard (retiredContexts_) keeps alive until ~AppController.
    GridContextFieldCatalog* catPtr = &fieldCatalog();
    LaunchBackgroundTask([this, representatives, componentProjectKeys, backend, catPtr,
                          trackerCfgForWorker = std::move(trackerCfgForWorker)]() mutable {
        WarmIssueTypeEditMetaWorker(representatives, componentProjectKeys, backend, catPtr,
                                    std::move(trackerCfgForWorker));
    });
}

void AppController::WarmIssueTypeEditMetaWorker(const std::vector<std::pair<std::string, std::string>>& representatives,
                                                const std::vector<std::string>& componentProjectKeys,
                                                const std::shared_ptr<ITrackerBackend>& backend,
                                                GridContextFieldCatalog* catPtr, TrackerConfig trackerCfgForWorker) {
    for (const auto& pair : representatives) {
        if (shuttingDown_.load()) {
            break;
        }
        std::string ignored;
        EnsureIssueEditMetaLoaded(pair.second, &ignored, nullptr, &trackerCfgForWorker);
    }

    ITrackerFieldCatalog* catalog = backend ? backend->FieldCatalog() : nullptr;
    if (catalog == nullptr) {
        return;
    }
    // Operate on the KICK-TIME catalog captured by the caller (#975) — NOT a completion-time
    // fieldCatalog() re-resolve, which would target whatever context is focused when the worker
    // finishes and leak this run's projectComponentsInFlight_ markers on the kick-time context.
    // The retired-context husk graveyard (retiredContexts_) keeps catPtr valid for the whole run.
    GridContextFieldCatalog& cat = *catPtr;
    for (const auto& projectKey : componentProjectKeys) {
        if (shuttingDown_.load()) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
            if (cat.projectComponentOptions_.find(projectKey) != cat.projectComponentOptions_.end()) {
                continue; // already warmed
            }
            // Respect a backoff recorded by a previous failed fetch (lazy or warm).
            const auto retryIt = cat.projectComponentsRetryAfter_.find(projectKey);
            if (retryIt != cat.projectComponentsRetryAfter_.end() &&
                std::chrono::steady_clock::now() < retryIt->second) {
                continue;
            }
            // Join the in-flight set so a concurrent lazy EnsureProjectComponentsLoaded for the
            // same project skips its own fetch (and vice-versa). Marker erased on EVERY exit
            // below (shutdown, fetch-fail, success), mirroring the lazy path's discipline.
            if (!cat.projectComponentsInFlight_.insert(projectKey).second) {
                continue; // a lazy fetch for this project is already running
            }
        }
        if (shuttingDown_.load()) {
            std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
            cat.projectComponentsInFlight_.erase(projectKey);
            break;
        }
        // Lock is NOT held across the HTTP call — fetch into locals, then lock-insert.
        auto componentsResult = catalog->FetchProjectComponents(trackerCfgForWorker, projectKey);
        if (!componentsResult) {
            LOG_DEBUG("AppController: per-project component warm skipped for %s: %s", projectKey.c_str(),
                      componentsResult.error().Detail.c_str());
            std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
            // Same backoff as the lazy path so a later open doesn't immediately re-hammer.
            cat.projectComponentsRetryAfter_[projectKey] = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            cat.projectComponentsInFlight_.erase(projectKey); // allow a later lazy open to retry
            continue;
        }
        std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
        cat.projectComponentOptions_[projectKey] = std::move(componentsResult.value().Options);
        cat.projectComponentsRetryAfter_.erase(projectKey); // success clears any prior backoff
        cat.projectComponentsInFlight_.erase(projectKey);
    }
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

bool AppController::CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                                         const TrackerField* fieldMeta, const std::string* issueTypeKeyOverride) const {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (!backend || issueId.empty() || fieldId.empty()) {
        return true;
    }
    if (IsEditableTimetrackingEstimateFieldId(fieldId)) {
        return true;
    }
    const TrackerField* meta = fieldMeta ? fieldMeta : FindFieldById(fieldId);
    if (meta && IsSprintField(*meta)) {
        return true;
    }
    const std::string fieldKey = ToLowerAsciiCopy(fieldId);
    // Edit metadata usually does not include a plain "set" for status; Jira applies changes via
    // POST /issue/{key}/transitions (see JiraClient::UpdateIssueFields).
    if (fieldKey == "status") {
        return true;
    }
    std::string issueTypeKey;
    if (issueTypeKeyOverride && !issueTypeKeyOverride->empty()) {
        issueTypeKey = *issueTypeKeyOverride;
    } else {
        issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    const auto it = issueEditMeta_.find(issueId);
    if (it == issueEditMeta_.end() || !it->second.loaded) {
        if (!issueTypeKey.empty()) {
            const auto byType = issueTypeEditMeta_.find(issueTypeKey);
            if (byType != issueTypeEditMeta_.end() && byType->second.loaded) {
                const auto typeFieldIt = byType->second.fieldCanEdit.find(fieldKey);
                if (typeFieldIt == byType->second.fieldCanEdit.end()) {
                    // Jira often omits `priority` (e.g. Epic) and `components` (cross-project / filter-id
                    // views) from editmeta while PUT still accepts them; force-editable carve-out. See
                    // AppController::CanEditFieldForIssue doc comment.
                    if (fieldKey == "priority" || fieldKey == "components") {
                        return true;
                    }
                    return false;
                }
                return typeFieldIt->second;
            }
        }
        return true;
    }
    const auto fieldIt = it->second.fieldCanEdit.find(fieldKey);
    if (fieldIt == it->second.fieldCanEdit.end()) {
        if (fieldKey == "priority" || fieldKey == "components") {
            return true;
        }
        return false;
    }
    return fieldIt->second;
}

bool AppController::EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError,
                                              const std::string* issueTypeKeyOverride,
                                              const TrackerConfig* configSnapshot) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (outError) {
        outError->clear();
    }
    if (!backend || issueId.empty()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto it = issueEditMeta_.find(issueId);
        if (it != issueEditMeta_.end() && it->second.loaded) {
            return true;
        }
    }
    std::string issueTypeKey;
    if (issueTypeKeyOverride && !issueTypeKeyOverride->empty()) {
        issueTypeKey = *issueTypeKeyOverride;
    } else {
        issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    }
    if (!issueTypeKey.empty()) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto typeIt = issueTypeEditMeta_.find(issueTypeKey);
        if (typeIt != issueTypeEditMeta_.end() && typeIt->second.loaded) {
            issueEditMeta_[issueId] = typeIt->second;
            return true;
        }
    }

    const TrackerConfig cfg = configSnapshot ? *configSnapshot : ConfigManager::Load();
    std::unordered_map<std::string, bool> meta;
    std::string fetchError;
    bool ok = false;
    if (backend->FieldCatalog() != nullptr) {
        auto metaResult = backend->FieldCatalog()->FetchIssueEditMeta(cfg, issueId);
        ok = static_cast<bool>(metaResult);
        if (ok) {
            meta = std::move(metaResult.value());
        } else {
            fetchError = metaResult.error().Detail;
        }
    }

    IssueEditMetaCache cache;
    // Only mark loaded after a successful fetch; on failure an empty map with loaded=true made
    // CanEditFieldForIssue deny every field (missing keys) instead of staying optimistic offline.
    cache.loaded = ok;
    if (ok) {
        cache.fieldCanEdit = std::move(meta);
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueEditMeta_[issueId] = cache;
        if (ok && !issueTypeKey.empty()) {
            issueTypeEditMeta_[issueTypeKey] = cache;
        }
    }

    if (!ok) {
        LOG_WARN("AppController: editmeta fetch failed issue=%s err=%s", issueId.c_str(), fetchError.c_str());
        if (outError) {
            *outError = fetchError;
        }
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::RefreshIssueEditMeta(const std::string& issueId, std::string* outError,
                                         const std::string* issueTypeKeyOverride) {
    std::string issueTypeKey;
    if (issueTypeKeyOverride && !issueTypeKeyOverride->empty()) {
        issueTypeKey = *issueTypeKeyOverride;
    } else {
        issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    }
    InvalidateIssueEditMeta(issueId);
    if (!issueTypeKey.empty()) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueTypeEditMeta_.erase(issueTypeKey);
    }
    return EnsureIssueEditMetaLoaded(issueId, outError, &issueTypeKey);
}

void AppController::InvalidateIssueEditMeta(const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    issueEditMeta_.erase(issueId);
}

void AppController::PruneEditMetaCacheToActiveTickets() {
    const auto ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    std::unordered_set<std::string> keep;
    std::unordered_set<std::string> keepTypes;
    keep.reserve(tickets.size());
    keepTypes.reserve(tickets.size());
    for (const auto& t : tickets) {
        if (!t.id.empty()) {
            keep.insert(t.id);
        }
        const std::string typeKey = ToLowerAsciiCopy(TrimCopy(t.GetFieldValue("issuetype")));
        if (!typeKey.empty()) {
            keepTypes.insert(typeKey);
        }
    }

    std::lock_guard<std::mutex> lock(editMetaMutex_);
    for (auto it = issueEditMeta_.begin(); it != issueEditMeta_.end();) {
        if (keep.find(it->first) == keep.end()) {
            it = issueEditMeta_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = issueTypeEditMeta_.begin(); it != issueTypeEditMeta_.end();) {
        if (keepTypes.find(it->first) == keepTypes.end()) {
            it = issueTypeEditMeta_.erase(it);
        } else {
            ++it;
        }
    }
}

void AppController::WarmIssueEditMetaAsync(const std::string& issueId) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    if (!backend || issueId.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto it = issueEditMeta_.find(issueId);
        if (it != issueEditMeta_.end() && it->second.loaded) {
            return;
        }
        if (issueEditMetaWarmupInFlight_.find(issueId) != issueEditMetaWarmupInFlight_.end()) {
            return;
        }
        issueEditMetaWarmupInFlight_.insert(issueId);
    }

    const TrackerConfig warmupTrackerCfg = ConfigManager::Load();
    LaunchBackgroundTask([this, issueId, warmupTrackerCfg]() {
        std::string ignored;
        EnsureIssueEditMetaLoaded(issueId, &ignored, nullptr, &warmupTrackerCfg);
        {
            std::lock_guard<std::mutex> lock(editMetaMutex_);
            issueEditMetaWarmupInFlight_.erase(issueId);
        }
    });
}

// SubmitFieldEdit branch helpers. SubmitFieldEditCtx is a private AppController struct (AppController.h).

bool AppController::SubmitFieldEditSprint(const SubmitFieldEditCtx& ctx, std::string& outError) {
    const std::string& issueId = ctx.issueId;
    const TrackerField& field = ctx.field;
    const auto& values = ctx.values;
    const auto& tickets = *ctx.ticketsSnap;
    ITrackerIssueMutations* mutations = ctx.mutations;
    const std::string& fieldEditAuditOp = ctx.fieldEditAuditOp;
    const char* const fieldEditAuditSource = ctx.fieldEditAuditSource;

    if (values.empty()) {
        outError = "Clearing sprint is not supported by this action.";
        LOG_WARN("AppController::SubmitFieldEdit sprint clear not supported issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }
    const std::string sprintId = values.front();
    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", field.Id}, {"kind", "sprint"}});
    const TrackerError sprintErr = mutations->AddIssueToSprint(issueId, sprintId);
    outError = sprintErr.Detail;
    if (!sprintErr.IsOk()) {
        LOG_ERROR("AppController::SubmitFieldEdit sprint update failed issue=%s field=%s sprint=%s err=%s",
                  issueId.c_str(), field.Id.c_str(), sprintId.c_str(), outError.c_str());
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
            nlohmann::json{{"field_id", field.Id},
                           {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                           {"after", sprintId}});
        return false;
    }
    if (ticketIt != tickets.end()) {
        CachedTicket updatedTicket = *ticketIt;
        std::string displayValue = sprintId;
        auto optIt = std::find_if(field.AllowedValueOptions.begin(), field.AllowedValueOptions.end(),
                                  [&](const auto& option) { return option.Id == sprintId; });
        if (optIt != field.AllowedValueOptions.end()) {
            displayValue = optIt->Value;
        }
        updatedTicket.fieldValues[field.Id] = displayValue;
        UpdateTicket(updatedTicket);
    } else {
        RefreshLocalData();
    }
    BackendAuditTrail::AppendResult(
        "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
        nlohmann::json{{"field_id", field.Id},
                       {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                       {"after", sprintId}});
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::SubmitFieldEditTimetracking(const SubmitFieldEditCtx& ctx, std::string& outError) {
    const std::string& issueId = ctx.issueId;
    const TrackerField& field = ctx.field;
    const auto& values = ctx.values;
    const auto& tickets = *ctx.ticketsSnap;
    ITrackerIssueMutations* mutations = ctx.mutations;
    const std::string& fieldEditAuditOp = ctx.fieldEditAuditOp;
    const char* const fieldEditAuditSource = ctx.fieldEditAuditSource;

    const std::string editedValue = values.empty() ? std::string() : values.front();
    if (editedValue.empty()) {
        outError = "Clearing Jira timetracking estimates is not supported by this editor.";
        LOG_WARN("AppController::SubmitFieldEdit blocked timetracking clear issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });

    std::string originalEstimate =
        (ticketIt != tickets.end()) ? ticketIt->GetFieldValue("timeoriginalestimate") : std::string();
    std::string remainingEstimate =
        (ticketIt != tickets.end()) ? ticketIt->GetFieldValue("timeestimate") : std::string();
    const std::string beforeOriginalEstimate = originalEstimate;
    const std::string beforeRemainingEstimate = remainingEstimate;
    if (field.Id == "timeoriginalestimate") {
        originalEstimate = editedValue;
    } else {
        remainingEstimate = editedValue;
    }

    nlohmann::json timetrackingPayload = nlohmann::json::object();
    if (!originalEstimate.empty()) {
        timetrackingPayload["originalEstimate"] = originalEstimate;
    }
    if (!remainingEstimate.empty()) {
        timetrackingPayload["remainingEstimate"] = remainingEstimate;
    }
    if (timetrackingPayload.empty()) {
        outError = "Timetracking update requires at least one estimate value.";
        return false;
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload["timetracking"] = std::move(timetrackingPayload);
    BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", "timetracking"}, {"kind", "timetracking"}});
    const TrackerError timetrackingErr = mutations->UpdateIssueFields(issueId, fieldsPayload);
    outError = timetrackingErr.Detail;
    if (!timetrackingErr.IsOk()) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) { // catch-all-ok: dump for logging
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR("AppController::SubmitFieldEdit failed issue=%s field=%s tracker_error=%s request=%s",
                  issueId.c_str(), field.Id.c_str(), outError.c_str(), TruncateForLog(payloadForLog, 1200).c_str());
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
            nlohmann::json{{"field_id", "timetracking"},
                           {"before", nlohmann::json{{"timeoriginalestimate", beforeOriginalEstimate},
                                                     {"timeestimate", beforeRemainingEstimate}}},
                           {"after", fieldsPayload["timetracking"]}});
        return false;
    }

    if (ticketIt != tickets.end()) {
        CachedTicket updatedTicket = *ticketIt;
        updatedTicket.fieldValues["timeoriginalestimate"] = originalEstimate;
        updatedTicket.fieldValues["timeestimate"] = remainingEstimate;
        UpdateTicket(updatedTicket);
    } else {
        RefreshLocalData();
    }
    BackendAuditTrail::AppendResult(
        "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
        nlohmann::json{{"field_id", "timetracking"},
                       {"before", nlohmann::json{{"timeoriginalestimate", beforeOriginalEstimate},
                                                 {"timeestimate", beforeRemainingEstimate}}},
                       {"after", fieldsPayload["timetracking"]}});
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::SubmitFieldEditRegular(const SubmitFieldEditCtx& ctx, std::string& outError) {
    const std::string& issueId = ctx.issueId;
    const TrackerField& field = ctx.field;
    const auto& rawValues = ctx.rawValues;
    const auto& values = ctx.values;
    const auto& tickets = *ctx.ticketsSnap;
    ITrackerIssueMutations* mutations = ctx.mutations;
    const std::shared_ptr<ITrackerBackend>& backend = ctx.backend;
    const std::string& fieldEditAuditOp = ctx.fieldEditAuditOp;
    const char* const fieldEditAuditSource = ctx.fieldEditAuditSource;

    EnsureIssueEditMetaLoaded(issueId, nullptr);

    if (!CanEditFieldForIssue(issueId, field.Id, &field)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        LOG_WARN("AppController::SubmitFieldEdit blocked by editmeta issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    auto fieldPayloadResult = mutations->BuildFieldPayload(field, rawValues);
    if (!fieldPayloadResult) {
        outError = fieldPayloadResult.error().Detail;
        LOG_WARN("AppController::SubmitFieldEdit invalid value issue=%s field=%s err=%s", issueId.c_str(),
                 field.Id.c_str(), outError.c_str());
        return false;
    }
    nlohmann::json fieldsPayload = std::move(fieldPayloadResult.value());

    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });

    BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", field.Id}, {"kind", "issue_fields"}});
    TrackerError updateErr = mutations->UpdateIssueFields(issueId, fieldsPayload);
    outError = updateErr.Detail;
    bool updateOk = updateErr.IsOk();
    bool didRetryAfter400 = false;
    if (!updateOk && ErrorTextContainsHttpStatus(outError, 400)) {
        didRetryAfter400 = true;
        RefreshIssueEditMeta(issueId, nullptr);
        if (!CanEditFieldForIssue(issueId, field.Id, &field)) {
            outError = "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            LOG_WARN("AppController::SubmitFieldEdit blocked after editmeta refresh issue=%s field=%s", issueId.c_str(),
                     field.Id.c_str());
            BackendAuditTrail::AppendResult(
                "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
                nlohmann::json{
                    {"field_id", field.Id},
                    {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                    {"after", rawValues}});
            return false;
        }
        updateErr = mutations->UpdateIssueFields(issueId, fieldsPayload);
        outError = updateErr.Detail;
        updateOk = updateErr.IsOk();
    }
    if (!updateOk) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) {
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR(
            "AppController::SubmitFieldEdit failed issue=%s field=%s retried_after_400=%d tracker_error=%s request=%s",
            issueId.c_str(), field.Id.c_str(), didRetryAfter400 ? 1 : 0, outError.c_str(),
            TruncateForLog(payloadForLog, 1200).c_str());
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
            nlohmann::json{{"field_id", field.Id},
                           {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                           {"after", rawValues}});
        return false;
    }

    // Keep local cache and in-memory model in sync with the successful backend update.
    if (ticketIt != tickets.end()) {
        CachedTicket updatedTicket = *ticketIt;

        std::string displayValue;
        if (!values.empty()) {
            for (size_t i = 0; i < values.size(); ++i) {
                if (i != 0) {
                    displayValue += ", ";
                }
                displayValue += backend->Reader().ResolveDisplayValue(field.Id, &field, values[i]);
            }
        }

        updatedTicket.fieldValues[field.Id] = displayValue;
        UpdateTicket(updatedTicket);
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{
                {"field_id", field.Id}, {"before", ticketIt->GetFieldValue(field.Id)}, {"after", displayValue}});
    } else {
        RefreshLocalData();
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{{"field_id", field.Id}, {"before", "unknown"}, {"after", rawValues}});
    }

    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues, std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::SubmitFieldEdit blocked by read-only mode issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }
    if (!backend || !Cache) {
        outError = "Backend or cache is not initialized.";
        LOG_WARN("AppController::SubmitFieldEdit skipped issue=%s field=%s: %s", issueId.c_str(), field.Id.c_str(),
                 outError.c_str());
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        LOG_WARN("AppController::SubmitFieldEdit skipped field=%s: %s", field.Id.c_str(), outError.c_str());
        return false;
    }

    ITrackerIssueMutations* const mutations = backend->Mutations();
    if (!mutations) {
        outError = "Tracker backend does not support issue mutations.";
        return false;
    }

    const std::string fieldEditAuditOp = BackendAuditTrail::MakeOperationId("field-edit");
    const char* const fieldEditAuditSource = FieldEditAuditSource::Current();
    LOG_TRACE("SubmitFieldEdit: source=%s issue=%s field=%s raw_values=%zu", fieldEditAuditSource, issueId.c_str(),
              field.Id.c_str(), rawValues.size());

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    const std::shared_ptr<const std::vector<CachedTicket>> ticketsSnap = GetActiveTicketsSnapshot();

    const SubmitFieldEditCtx ctx{
        issueId, field, rawValues, values, mutations, backend, ticketsSnap, fieldEditAuditOp, fieldEditAuditSource};

    if (IsSprintField(field)) {
        return SubmitFieldEditSprint(ctx, outError);
    }

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outError = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        LOG_WARN("AppController::SubmitFieldEdit blocked non-editable timetracking issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        return SubmitFieldEditTimetracking(ctx, outError);
    }

    return SubmitFieldEditRegular(ctx, outError);
}

bool AppController::SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues,
                                               const std::string& originalEstimateSnapshot,
                                               const std::string& remainingEstimateSnapshot,
                                               const std::string& issueTypeKeySnapshot, FieldEditResult& outResult) {
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &focusedContext()
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
    outResult = FieldEditResult{};
    LOG_TRACE("SubmitFieldEditNetworkOnly: source=%s issue=%s field=%s raw_values=%zu", FieldEditAuditSource::Current(),
              issueId.c_str(), field.Id.c_str(), rawValues.size());
    if (ConfigManager::Load().ReadOnlyMode) {
        outResult.Error = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::SubmitFieldEditNetworkOnly blocked by read-only mode issue=%s field=%s",
                 issueId.c_str(), field.Id.c_str());
        return false;
    }
    if (issueId.empty()) {
        outResult.Error = "Issue id is empty.";
        return false;
    }

    if (!backend) {
        outResult.Error = "No tracker backend initialized.";
        return false;
    }
    ITrackerIssueMutations* const mutations = backend->Mutations();
    if (!mutations) {
        outResult.Error = "Tracker backend does not support issue mutations.";
        return false;
    }
    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    if (IsSprintField(field)) {
        bool handled = false;
        const bool ok = SubmitSprintFieldEditNetworkOnly(issueId, field, values, *mutations, outResult, handled);
        if (handled) {
            return ok;
        }
    }

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outResult.Error = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        return false;
    }

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        bool handled = false;
        const bool ok =
            SubmitTimetrackingFieldEditNetworkOnly(issueId, field, values, originalEstimateSnapshot,
                                                   remainingEstimateSnapshot, *mutations, outResult, handled);
        if (handled) {
            return ok;
        }
    }

    const std::string* issueTypeKeyOpt = issueTypeKeySnapshot.empty() ? nullptr : &issueTypeKeySnapshot;

    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(issueId, field, rawValues, originalEstimateSnapshot,
                                            remainingEstimateSnapshot, issueTypeKeySnapshot, fieldsPayload,
                                            displayValues, outResult.Error)) {
        return false;
    }

    if (!ApplyFieldUpdateWithEditMetaRetry(issueId, field, fieldsPayload, issueTypeKeyOpt, *mutations, outResult)) {
        return false;
    }

    outResult.Ok = true;
    outResult.UpdatedDisplayValues = std::move(displayValues);
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::ApplyFieldUpdateWithEditMetaRetry(const std::string& issueId, const TrackerField& field,
                                                      const nlohmann::json& fieldsPayload,
                                                      const std::string* issueTypeKeyOpt,
                                                      ITrackerIssueMutations& mutations, FieldEditResult& outResult) {
    TrackerError updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
    outResult.Error = updateErr.Detail;
    bool updateOk = updateErr.IsOk();
    bool didRetryAfter400 = false;
    if (!updateOk && ErrorTextContainsHttpStatus(outResult.Error, 400)) {
        didRetryAfter400 = true;
        RefreshIssueEditMeta(issueId, nullptr, issueTypeKeyOpt);
        if (!CanEditFieldForIssue(issueId, field.Id, &field, issueTypeKeyOpt)) {
            outResult.Error =
                "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            LOG_WARN("AppController::SubmitFieldEditNetworkOnly blocked after editmeta refresh issue=%s field=%s",
                     issueId.c_str(), field.Id.c_str());
            return false;
        }
        updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
        outResult.Error = updateErr.Detail;
        updateOk = updateErr.IsOk();
    }
    if (!updateOk) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) {
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR("AppController::SubmitFieldEditNetworkOnly failed issue=%s field=%s retried_after_400=%d "
                  "tracker_error=%s request=%s",
                  issueId.c_str(), field.Id.c_str(), didRetryAfter400 ? 1 : 0, outResult.Error.c_str(),
                  TruncateForLog(payloadForLog, 1200).c_str());
        return false;
    }
    return true;
}

bool AppController::SubmitSprintFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                                     const std::vector<std::string>& values,
                                                     ITrackerIssueMutations& mutations, FieldEditResult& outResult,
                                                     bool& handled) {
    handled = true;
    if (values.empty()) {
        outResult.Error = "Clearing sprint is not supported by this action.";
        return false;
    }
    const std::string sprintId = values.front();
    const TrackerError sprintErr = mutations.AddIssueToSprint(issueId, sprintId);
    if (!sprintErr.IsOk()) {
        outResult.Error = sprintErr.Detail;
        return false;
    }
    std::string displayValue = sprintId;
    auto optIt = std::find_if(field.AllowedValueOptions.begin(), field.AllowedValueOptions.end(),
                              [&](const auto& option) { return option.Id == sprintId; });
    if (optIt != field.AllowedValueOptions.end()) {
        displayValue = optIt->Value;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues[field.Id] = std::move(displayValue);
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::SubmitTimetrackingFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                                           const std::vector<std::string>& values,
                                                           const std::string& originalEstimateSnapshot,
                                                           const std::string& remainingEstimateSnapshot,
                                                           ITrackerIssueMutations& mutations,
                                                           FieldEditResult& outResult, bool& handled) {
    handled = true;
    const std::string editedValue = values.empty() ? std::string() : values.front();
    if (editedValue.empty()) {
        outResult.Error = "Clearing Jira timetracking estimates is not supported by this editor.";
        return false;
    }
    std::string originalEstimate = originalEstimateSnapshot;
    std::string remainingEstimate = remainingEstimateSnapshot;
    if (field.Id == "timeoriginalestimate") {
        originalEstimate = editedValue;
    } else {
        remainingEstimate = editedValue;
    }

    nlohmann::json timetrackingPayload = nlohmann::json::object();
    if (!originalEstimate.empty()) {
        timetrackingPayload["originalEstimate"] = originalEstimate;
    }
    if (!remainingEstimate.empty()) {
        timetrackingPayload["remainingEstimate"] = remainingEstimate;
    }
    if (timetrackingPayload.empty()) {
        outResult.Error = "Timetracking update requires at least one estimate value.";
        return false;
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload["timetracking"] = std::move(timetrackingPayload);
    const TrackerError updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
    if (!updateErr.IsOk()) {
        outResult.Error = updateErr.Detail;
        return false;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues["timeoriginalestimate"] = std::move(originalEstimate);
    outResult.UpdatedDisplayValues["timeestimate"] = std::move(remainingEstimate);
    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::TryPrepareOfflineFieldEdit(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues,
                                               const std::string& originalEstimateSnapshot,
                                               const std::string& remainingEstimateSnapshot,
                                               const std::string& issueTypeKeySnapshot, FieldEditResult& outResult,
                                               std::string& outFieldsPayloadJson, std::string& outError) {
    outResult = FieldEditResult{};
    outFieldsPayloadJson.clear();
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        return false;
    }
    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(issueId, field, rawValues, originalEstimateSnapshot,
                                            remainingEstimateSnapshot, issueTypeKeySnapshot, fieldsPayload,
                                            displayValues, outError)) {
        return false;
    }
    try {
        outFieldsPayloadJson = fieldsPayload.dump();
    } catch (const std::exception& ex) {
        outError = ex.what();
        return false;
    } catch (...) {
        LOG_WARN("BuildFieldEditPayload: unknown exception serializing field payload");
        outError = "Failed to serialize field payload.";
        return false;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues = std::move(displayValues);
    return true;
}

bool AppController::ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result,
                                         std::string& outError) {
    outError.clear();
    if (!result.Ok) {
        outError = result.Error.empty() ? std::string("Failed to save field update.") : result.Error;
        return false;
    }
    if (!Cache) {
        outError = "Cache is not initialized.";
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }

    const auto ticketsSnapApply = GetActiveTicketsSnapshot();
    const auto& ticketsApply = *ticketsSnapApply;
    auto ticketIt = std::find_if(ticketsApply.begin(), ticketsApply.end(),
                                 [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (ticketIt == ticketsApply.end()) {
        RefreshLocalData();
        return true;
    }

    CachedTicket updatedTicket = *ticketIt;
    for (const auto& pair : result.UpdatedDisplayValues) {
        updatedTicket.fieldValues[pair.first] = pair.second;
    }
    UpdateTicket(updatedTicket);
    return true;
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
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(
        &paneContextOrFocused_(paneId)
             .Backend); // latch: live tracker swap (SetBackend) must not free the backend mid-call (ADR 0012)
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
        // Scope the context reference: it must NOT be held across the blocking fetch below —
        // a hidden-pane retirement mid-fetch would dangle it. The latched backend shared_ptr
        // is the only thing carried across (ADR 0012).
        GridLiveContext& ctx = paneContextOrFocused_(paneId);
        {
            std::lock_guard<std::mutex> lock(ctx.groupRoster.rosterMutex_);
            std::unordered_map<std::string, std::vector<TrackerUser>>::const_iterator hit =
                ctx.groupRoster.MembersByGroup.find(groupName);
            if (hit != ctx.groupRoster.MembersByGroup.end()) {
                outMembers = hit->second;
                return true;
            }
        }
        backend = std::atomic_load(&ctx.Backend);
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
    std::map<std::string, std::unique_ptr<GridLiveContext>>::iterator it = gridContexts_.find(paneId);
    if (it != gridContexts_.end()) {
        std::lock_guard<std::mutex> lock(it->second->groupRoster.rosterMutex_);
        if (ok) {
            it->second->groupRoster.MembersByGroup[groupName] = outMembers;
            it->second->groupRoster.LastGroupRosterError.clear();
        } else {
            it->second->groupRoster.LastGroupRosterError = outError;
        }
    }
    return ok;
}
