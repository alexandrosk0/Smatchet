#include "EditMetaCacheService.h"

#include "IEditMetaDeps.h"
#include "GridLiveContext.h" // full GridContextFieldCatalog definition (#975 per-project component write)
#include "ITrackerBackend.h"
#include "ITrackerFieldCatalog.h" // full def: FetchIssueEditMeta / FetchProjectComponents
#include "Config/ConfigManager.h"
#include "Tracker/ProjectResolver.h" // smatchet::ExtractIssueKeyPrefix
#include "TrackerFieldValueUtils.h"
#include "TrackerFieldSchema.h"
#include "CachedTicketTypes.h"

#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// COPIED (not moved) from AppController_CatalogAndFieldEdit.cpp: the original definitions stay
// there because the SubmitFieldEdit* family (Phase 2) still uses them. Anonymous-namespace
// internal linkage in both TUs → no ODR clash.
bool IsSprintField(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::Sprint || field.SchemaCustom.find("gh-sprint") != std::string::npos;
}

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

void EditMetaCacheService::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend) {
        return;
    }
    const auto ticketsSnap = deps_.GetActiveTicketsSnapshot();
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
    GridContextFieldCatalog* catPtr = deps_.KickTimeFieldCatalog();
    deps_.LaunchBackgroundTask([this, representatives, componentProjectKeys, backend, catPtr,
                                trackerCfgForWorker = std::move(trackerCfgForWorker)]() mutable {
        WarmIssueTypeEditMetaWorker(representatives, componentProjectKeys, backend, catPtr,
                                    std::move(trackerCfgForWorker));
    });
}

void EditMetaCacheService::WarmIssueTypeEditMetaWorker(
    const std::vector<std::pair<std::string, std::string>>& representatives,
    const std::vector<std::string>& componentProjectKeys, const std::shared_ptr<ITrackerBackend>& backend,
    GridContextFieldCatalog* catPtr, TrackerConfig trackerCfgForWorker) {
    for (const auto& pair : representatives) {
        if (deps_.IsShuttingDown()) {
            break;
        }
        // Fire-and-forget warmup: an editmeta fetch failure here is intentionally ignored (the
        // issue stays optimistic) — discard the VoidResult.
        EnsureIssueEditMetaLoaded(pair.second, nullptr, &trackerCfgForWorker);
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
        if (deps_.IsShuttingDown()) {
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
        if (deps_.IsShuttingDown()) {
            std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
            cat.projectComponentsInFlight_.erase(projectKey);
            break;
        }
        // Lock is NOT held across the HTTP call — fetch into locals, then lock-insert.
        auto componentsResult = catalog->FetchProjectComponents(trackerCfgForWorker, projectKey);
        if (!componentsResult) {
            LOG_DEBUG("EditMetaCacheService: per-project component warm skipped for %s: %s", projectKey.c_str(),
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

bool EditMetaCacheService::CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                                                const TrackerField* fieldMeta,
                                                const std::string* issueTypeKeyOverride) const {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || issueId.empty() || fieldId.empty()) {
        return true;
    }
    if (IsEditableTimetrackingEstimateFieldId(fieldId)) {
        return true;
    }
    const TrackerField* meta = fieldMeta ? fieldMeta : deps_.FindFieldById(fieldId);
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

VoidResult EditMetaCacheService::EnsureIssueEditMetaLoaded(const std::string& issueId,
                                                           const std::string* issueTypeKeyOverride,
                                                           const TrackerConfig* configSnapshot) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || issueId.empty()) {
        return VoidOk();
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto it = issueEditMeta_.find(issueId);
        if (it != issueEditMeta_.end() && it->second.loaded) {
            return VoidOk();
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
            return VoidOk();
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
        LOG_WARN("EditMetaCacheService: editmeta fetch failed issue=%s err=%s", issueId.c_str(), fetchError.c_str());
        return VoidResult::Err(fetchError);
    }
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return VoidOk();
}

VoidResult EditMetaCacheService::RefreshIssueEditMeta(const std::string& issueId,
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
    return EnsureIssueEditMetaLoaded(issueId, &issueTypeKey);
}

void EditMetaCacheService::InvalidateIssueEditMeta(const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    issueEditMeta_.erase(issueId);
}

void EditMetaCacheService::PruneEditMetaCacheToActiveTickets() {
    const auto ticketsSnap = deps_.GetActiveTicketsSnapshot();
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

void EditMetaCacheService::WarmIssueEditMetaAsync(const std::string& issueId) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
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
    deps_.LaunchBackgroundTask([this, issueId, warmupTrackerCfg]() {
        // Best-effort async warmup: ignore fetch failure (issue stays optimistic) — discard VoidResult.
        EnsureIssueEditMetaLoaded(issueId, nullptr, &warmupTrackerCfg);
        {
            std::lock_guard<std::mutex> lock(editMetaMutex_);
            issueEditMetaWarmupInFlight_.erase(issueId);
        }
    });
}
