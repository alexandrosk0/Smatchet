#include "EditMetaCacheService.h"

#include "TrackerFieldPayloadPure.h"

#include "IEditMetaDeps.h"
#include "ILookupCache.h"
#include "ITrackerBackend.h"
#include "ITrackerFieldCatalog.h" // full def: FetchIssueEditMeta
#include "LearnedWorkflowPure.h"  // BuildScopedKey (in-flight key)
#include "LookupPayloadsPure.h"   // kEditMetaTypeKind + the permissions payload codec
#include "Config/ConfigManager.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerFieldSchema.h"
#include "CachedTicketTypes.h"

#include "Logger.h"
#include "OfflineFirstPure.h" // IsOfflineState, kLookupRetryAfterSeconds (Pillar 6 gate + backoff)
#include "ScopeExit.h"
#include "StringUtil.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsEditableTimetrackingEstimateFieldId(fieldId);
}

} // namespace

EditMetaCacheService::EditMetaCacheService(IEditMetaDeps& deps) : deps_(deps) {}

std::string EditMetaCacheService::ResolveIssueTypeKeyForIssue(const std::string& issueId) const {
    if (issueId.empty()) {
        return std::string();
    }
    const auto ticketsSnap = deps_.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    const auto it =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (it == tickets.end()) {
        return std::string();
    }
    return ToLowerAsciiCopy(TrimCopy(it->GetFieldValue("issuetype")));
}

const EditMetaCacheService::IssueEditMetaCache*
EditMetaCacheService::FindLoaded(const EditMetaByBackend& maps, const std::string& backendKey, const std::string& id) {
    const auto backendIt = maps.find(backendKey);
    if (backendIt == maps.end()) {
        return nullptr;
    }
    const auto it = backendIt->second.find(id);
    return (it != backendIt->second.end() && it->second.loaded) ? &it->second : nullptr;
}

std::string EditMetaCacheService::InFlightKey(const std::string& backendKey, const std::string& issueId) {
    return smatchet::workflow::BuildScopedKey(backendKey, issueId);
}

void EditMetaCacheService::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend) {
        return;
    }
    const std::string backendKey = deps_.CacheBackendKey();
    EnsureSavedTypesLoaded(backendKey);
    const auto ticketsSnap = deps_.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    std::vector<std::pair<std::string, std::string>> representatives;
    std::unordered_set<std::string> seenTypes;
    seenTypes.reserve(tickets.size());
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        for (const auto& ticket : tickets) {
            if (ticket.id.empty()) {
                continue;
            }
            const std::string typeKey = ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
            if (typeKey.empty() || !seenTypes.insert(typeKey).second) {
                continue;
            }
            // A restored (not live) entry still gets one live fetch per session.
            const IssueEditMetaCache* typeMeta = FindLoaded(issueTypeEditMeta_, backendKey, typeKey);
            if (typeMeta != nullptr && typeMeta->live) {
                continue;
            }
            representatives.push_back({typeKey, ticket.id});
        }
    }
    if (representatives.empty()) {
        return;
    }
    try {
        deps_.LaunchBackgroundTask(
            [this, representatives, backend, backendKey, cfg = std::move(trackerCfgForWorker)]() {
                WarmIssueTypeEditMetaWorker(representatives, backend, backendKey, cfg);
            });
    } catch (const std::exception& ex) {
        LOG_WARN("EditMetaCacheService: issue-type editmeta warm did not start: %s", ex.what());
    }
}

void EditMetaCacheService::WarmIssueTypeEditMetaWorker(
    const std::vector<std::pair<std::string, std::string>>& representatives,
    const std::shared_ptr<ITrackerBackend>& backend, const std::string& backendKey,
    const TrackerConfig& trackerCfgForWorker) {
    for (const auto& pair : representatives) {
        if (deps_.IsShuttingDown()) {
            break;
        }
        // Fire-and-forget warmup: an editmeta fetch failure here is intentionally ignored (the
        // issue stays optimistic) — discard the VoidResult.
        EnsureIssueEditMetaLoadedFor(backend, backendKey, pair.second, pair.first, &trackerCfgForWorker);
    }
}

void EditMetaCacheService::EnsureSavedTypesLoaded(const std::string& backendKey) {
    // SQLite read and JSON parse on a worker; editMetaMutex_ is taken only to apply the parsed rows.
    smatchet::offline::LoadSavedRowsOnce(
        deps_, savedTypesLoad_, backendKey, smatchet::lookup::kEditMetaTypeKind, "saved edit permissions",
        [this](const std::string& key, const std::vector<LookupCacheRow>& rows) {
            EditMetaById restored;
            restored.reserve(rows.size());
            for (const LookupCacheRow& row : rows) {
                IssueEditMetaCache entry;
                entry.loaded = !row.CacheKey.empty() &&
                               smatchet::lookup::ParseEditPermissions(row.PayloadJson, entry.fieldCanEdit);
                if (entry.loaded) {
                    restored.emplace(row.CacheKey, std::move(entry)); // live stays false: a saved copy
                }
            }
            std::lock_guard<std::mutex> lock(editMetaMutex_);
            EditMetaById& byType = issueTypeEditMeta_[key];
            for (auto& kv : restored) {
                // emplace never replaces an entry fetched live meanwhile.
                byType.emplace(kv.first, std::move(kv.second));
            }
        });
}

void EditMetaCacheService::SaveTypeEditMeta(const std::string& backendKey, const std::string& issueTypeKey,
                                            const std::unordered_map<std::string, bool>& fieldCanEdit) {
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    if (!store) {
        return;
    }
    try {
        store->UpsertLookup(backendKey, smatchet::lookup::kEditMetaTypeKind, issueTypeKey,
                            smatchet::lookup::SerializeEditPermissions(fieldCanEdit));
    } catch (const std::exception& ex) {
        LOG_WARN("EditMetaCacheService: saving edit permissions for type %s failed: %s", issueTypeKey.c_str(),
                 ex.what());
    }
}

bool EditMetaCacheService::CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                                                const TrackerField* fieldMeta,
                                                const std::string* issueTypeKeyOverride) const {
    if (!deps_.BackendShared()) {
        return true;
    }
    const bool haveOverride = issueTypeKeyOverride != nullptr && !issueTypeKeyOverride->empty();
    return CanEditFieldImpl(deps_.CacheBackendKey(), issueId, fieldId, fieldMeta,
                            haveOverride ? issueTypeKeyOverride : nullptr);
}

bool EditMetaCacheService::CanEditFieldForIssueWithType(const std::string& backendKey, const std::string& issueId,
                                                        const std::string& fieldId, const TrackerField* fieldMeta,
                                                        const std::string& issueTypeKey) const {
    return CanEditFieldImpl(backendKey, issueId, fieldId, fieldMeta, &issueTypeKey);
}

bool EditMetaCacheService::CanEditFieldImpl(const std::string& backendKey, const std::string& issueId,
                                            const std::string& fieldId, const TrackerField* fieldMeta,
                                            const std::string* explicitTypeKey) const {
    if (issueId.empty() || fieldId.empty()) {
        return true;
    }
    if (IsEditableTimetrackingEstimateFieldId(fieldId)) {
        return true;
    }
    const TrackerField* meta = fieldMeta ? fieldMeta : deps_.FindFieldById(fieldId);
    if (meta && TrackerFieldPayloadPure::IsSprintField(*meta)) {
        return true;
    }
    const std::string fieldKey = ToLowerAsciiCopy(fieldId);
    // Edit metadata usually does not include a plain "set" for status; Jira applies changes via
    // POST /issue/{key}/transitions (see JiraClient::UpdateIssueFields).
    if (fieldKey == "status") {
        return true;
    }
    const std::string issueTypeKey = explicitTypeKey ? *explicitTypeKey : ResolveIssueTypeKeyForIssue(issueId);
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    // The issue's own editmeta, else its issue type's (live or restored), else optimistic.
    const IssueEditMetaCache* loaded = FindLoaded(issueEditMeta_, backendKey, issueId);
    if (loaded == nullptr && !issueTypeKey.empty()) {
        loaded = FindLoaded(issueTypeEditMeta_, backendKey, issueTypeKey);
    }
    if (loaded == nullptr) {
        return true;
    }
    const auto fieldIt = loaded->fieldCanEdit.find(fieldKey);
    if (fieldIt == loaded->fieldCanEdit.end()) {
        // Jira often omits `priority` (e.g. Epic) and `components` (cross-project / filter-id views)
        // from editmeta while PUT still accepts them; force-editable carve-out. See
        // AppController::CanEditFieldForIssue doc comment.
        return fieldKey == "priority" || fieldKey == "components";
    }
    return fieldIt->second;
}

VoidResult EditMetaCacheService::EnsureIssueEditMetaLoaded(const std::string& issueId,
                                                           const std::string* issueTypeKeyOverride,
                                                           const TrackerConfig* configSnapshot) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || issueId.empty()) {
        return VoidOk();
    }
    const std::string backendKey = deps_.CacheBackendKey();
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        if (FindLoaded(issueEditMeta_, backendKey, issueId) != nullptr) {
            return VoidOk(); // skip the issue-type scan below
        }
    }
    const std::string issueTypeKey = (issueTypeKeyOverride && !issueTypeKeyOverride->empty())
                                         ? *issueTypeKeyOverride
                                         : ResolveIssueTypeKeyForIssue(issueId);
    return EnsureIssueEditMetaLoadedFor(backend, backendKey, issueId, issueTypeKey, configSnapshot);
}

VoidResult EditMetaCacheService::EnsureIssueEditMetaLoadedFor(const std::shared_ptr<ITrackerBackend>& backend,
                                                              const std::string& backendKey, const std::string& issueId,
                                                              const std::string& issueTypeKey,
                                                              const TrackerConfig* configSnapshot) {
    if (!backend || issueId.empty()) {
        return VoidOk();
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        if (FindLoaded(issueEditMeta_, backendKey, issueId) != nullptr) {
            return VoidOk();
        }
        // A type entry fetched this session stands in for the issue. A restored one does not: it keeps
        // answering CanEdit* offline, but a reachable tracker is asked for the issue's own editmeta.
        const IssueEditMetaCache* typeMeta =
            issueTypeKey.empty() ? nullptr : FindLoaded(issueTypeEditMeta_, backendKey, issueTypeKey);
        if (typeMeta != nullptr && typeMeta->live) {
            issueEditMeta_[backendKey][issueId] = *typeMeta;
            return VoidOk();
        }
    }

    if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) {
        return VoidResult::Err("Tracker is offline; edit permissions were not refreshed.");
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
    cache.live = ok;
    if (ok) {
        cache.fieldCanEdit = std::move(meta);
    } else {
        cache.retryAfter =
            std::chrono::steady_clock::now() + std::chrono::seconds(smatchet::offline::kLookupRetryAfterSeconds);
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueEditMeta_[backendKey][issueId] = cache;
        if (ok && !issueTypeKey.empty()) {
            issueTypeEditMeta_[backendKey][issueTypeKey] = cache;
        }
    }

    if (!ok) {
        LOG_WARN("EditMetaCacheService: editmeta fetch failed issue=%s err=%s", issueId.c_str(), fetchError.c_str());
        return VoidResult::Err(fetchError);
    }
    if (!issueTypeKey.empty()) {
        SaveTypeEditMeta(backendKey, issueTypeKey, cache.fieldCanEdit); // this thread made a network call: a worker
    }
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return VoidOk();
}

VoidResult EditMetaCacheService::RefreshIssueEditMeta(const std::string& issueId,
                                                      const std::string* issueTypeKeyOverride) {
    const std::string issueTypeKey = (issueTypeKeyOverride && !issueTypeKeyOverride->empty())
                                         ? *issueTypeKeyOverride
                                         : ResolveIssueTypeKeyForIssue(issueId);
    return RefreshIssueEditMetaFor(deps_.BackendShared(), deps_.CacheBackendKey(), issueId, issueTypeKey);
}

VoidResult EditMetaCacheService::RefreshIssueEditMetaFor(const std::shared_ptr<ITrackerBackend>& backend,
                                                         const std::string& backendKey, const std::string& issueId,
                                                         const std::string& issueTypeKey) {
    InvalidateIssueEditMetaFor(backendKey, issueId);
    if (!issueTypeKey.empty()) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto backendIt = issueTypeEditMeta_.find(backendKey);
        if (backendIt != issueTypeEditMeta_.end()) {
            backendIt->second.erase(issueTypeKey);
        }
    }
    return EnsureIssueEditMetaLoadedFor(backend, backendKey, issueId, issueTypeKey);
}

void EditMetaCacheService::InvalidateIssueEditMeta(const std::string& issueId) {
    InvalidateIssueEditMetaFor(deps_.CacheBackendKey(), issueId);
}

void EditMetaCacheService::InvalidateIssueEditMetaFor(const std::string& backendKey, const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    const auto backendIt = issueEditMeta_.find(backendKey);
    if (backendIt != issueEditMeta_.end()) {
        backendIt->second.erase(issueId);
    }
}

void EditMetaCacheService::PruneEditMetaCacheToActiveTickets() {
    // Union across EVERY live pane, not just the calling one: this cache is process-wide, so a
    // prune scoped to one pane's roster evicts the entries another pane's open editor is using and
    // forces a fresh editmeta round-trip on its next keystroke (multi-pane scoping audit).
    const std::vector<std::shared_ptr<const std::vector<CachedTicket>>> snaps =
        deps_.GetActiveTicketsSnapshotsAllPanes();
    std::unordered_set<std::string> keep;
    std::unordered_set<std::string> keepTypes;
    for (std::size_t s = 0; s < snaps.size(); ++s) {
        if (!snaps[s]) {
            continue;
        }
        const std::vector<CachedTicket>& tickets = *snaps[s];
        keep.reserve(keep.size() + tickets.size());
        keepTypes.reserve(keepTypes.size() + tickets.size());
        for (const auto& t : tickets) {
            if (!t.id.empty()) {
                keep.insert(t.id);
            }
            const std::string typeKey = ToLowerAsciiCopy(TrimCopy(t.GetFieldValue("issuetype")));
            if (!typeKey.empty()) {
                keepTypes.insert(typeKey);
            }
        }
    }

    // The union holds bare ids (the snapshots do not carry their pane's backend namespace), so an id
    // is kept in every namespace where it is active in some pane: a prune may keep a little extra,
    // never drop an entry an open editor uses.
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    for (auto& byBackend : issueEditMeta_) {
        EditMetaById& byId = byBackend.second;
        for (auto it = byId.begin(); it != byId.end();) {
            it = keep.find(it->first) == keep.end() ? byId.erase(it) : std::next(it);
        }
    }
    for (auto& byBackend : issueTypeEditMeta_) {
        EditMetaById& byType = byBackend.second;
        for (auto it = byType.begin(); it != byType.end();) {
            // A restored entry is kept: it is loaded once per backend, so pruning it would lose the
            // type's saved permissions for the rest of the session.
            const bool drop = it->second.live && keepTypes.find(it->first) == keepTypes.end();
            it = drop ? byType.erase(it) : std::next(it);
        }
    }
}

void EditMetaCacheService::WarmIssueEditMetaAsync(const std::string& issueId) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || issueId.empty()) {
        return;
    }
    if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) {
        return; // Pillar 6: no fetch while offline; the issue stays optimistic
    }
    const std::string backendKey = deps_.CacheBackendKey();
    const std::string inFlightKey = InFlightKey(backendKey, issueId);
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto backendIt = issueEditMeta_.find(backendKey);
        if (backendIt != issueEditMeta_.end()) {
            const auto it = backendIt->second.find(issueId);
            if (it != backendIt->second.end() && it->second.loaded) {
                return;
            }
            if (it != backendIt->second.end() && std::chrono::steady_clock::now() < it->second.retryAfter) {
                return; // failed recently — back off instead of refetching every frame
            }
        }
        if (!issueEditMetaWarmupInFlight_.insert(inFlightKey).second) {
            return;
        }
    }

    // Resolved now, on the UI thread, with the backend and namespace above: the worker loads the
    // pane that kicked it even if focus moves before it runs.
    const std::string issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    const TrackerConfig warmupTrackerCfg = ConfigManager::Load();
    try {
        deps_.LaunchBackgroundTask([this, backend, backendKey, issueId, issueTypeKey, inFlightKey, warmupTrackerCfg]() {
            // Clear the in-flight marker on every exit, a throw included, so a later frame can retry.
            smatchet::ScopeExit clearInFlight([this, &inFlightKey]() {
                std::lock_guard<std::mutex> lock(editMetaMutex_);
                issueEditMetaWarmupInFlight_.erase(inFlightKey);
            });
            // Best-effort async warmup: ignore fetch failure (issue stays optimistic) — discard VoidResult.
            EnsureIssueEditMetaLoadedFor(backend, backendKey, issueId, issueTypeKey, &warmupTrackerCfg);
        });
    } catch (const std::exception& ex) {
        LOG_WARN("EditMetaCacheService: editmeta warmup for %s did not start: %s", issueId.c_str(), ex.what());
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueEditMetaWarmupInFlight_.erase(inFlightKey);
    }
}

void EditMetaCacheService::OnConnectivityRecovered() {
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        for (auto& byBackend : issueEditMeta_) {
            for (auto& kv : byBackend.second) {
                kv.second.retryAfter = std::chrono::steady_clock::time_point();
            }
        }
    }
    savedTypesLoad_.ClearBackoff();
}
