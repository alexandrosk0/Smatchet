#include "AppController.h"

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
#include "JiraClient.h"
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

void AppController::RefreshLocalData() {
    if (Cache) {
        auto latestTickets = Cache->GetAllTickets();
        {
            std::lock_guard<std::mutex> lock(activeTicketsMutex_);
            ActiveTickets = std::move(latestTickets);
            activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ActiveTickets);
        }
        PruneEditMetaCacheToActiveTickets();
        ActiveTicketsRevision.fetch_add(1);
    }
}

void AppController::RefreshLocalDataAndWarmIssueTypeMeta() {
    RefreshLocalData();
    if (Backend) {
        WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());
    }
}

void AppController::UpdateTicket(const CachedTicket& ticket) {
    if (Cache) {
        Cache->SaveTicket(ticket);
        RefreshLocalData(); // Push changes back to ActiveTickets vector
    }
}

bool AppController::RefreshFieldCatalog(const TrackerConfig& cfg) {
    if (!Backend) {
        SetFieldCatalog({}, {}, "Tracker backend is not initialized.");
        return false;
    }

    TrackerFieldCatalogResult catalog;
    std::string error;
    const bool ok = Backend->FetchFieldCatalog(cfg, catalog, error);
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
    outCatalog = TrackerFieldCatalogResult{};
    outError.clear();
    if (!Backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    return Backend->FetchFieldCatalog(cfg, outCatalog, outError);
}

std::string AppController::BuildIssueBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const {
    return Backend ? Backend->BuildBrowseUrl(cfg, issueKey) : std::string();
}

std::string AppController::ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                               const std::string& value) const {
    if (!Backend) {
        return value;
    }
    return Backend->ResolveDisplayValue(fieldId, field, value);
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

void AppController::SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                                    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error) {
    const TrackerConfig cfgSnap = ConfigManager::Load();
    const std::string catalogCacheKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfgSnap);
    const bool catalogPlane = ConfigManager::NormalizeViewsBackendKey(cfgSnap.TrackerType) == "Plane";

    if (!error.empty()) {
        if (IsTrackerTransportErrorText(error)) {
            if (!AvailableFields.empty()) {
                LastTrackerFieldCatalogError.clear();
                const std::string nextWarning =
                    std::string("Offline: using cached ") + (catalogPlane ? "Plane" : "Jira") +
                    " field catalog. Last fetch failed: " + error;
                if (nextWarning != LastTrackerFieldCatalogWarning) {
                    LastTrackerFieldCatalogWarning = nextWarning;
                    TrackerFieldCatalogRevision.fetch_add(1);
                }
                LOG_WARN("AppController::SetFieldCatalog transport failure (catalog preserved): %s", error.c_str());
                return;
            }

            std::vector<TrackerField> snapFields;
            std::vector<TrackerComponent> snapComponents;
            std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta;
            std::string snapErr;
            if (FieldCatalogCache::TryLoadFieldCatalogSnapshot(catalogCacheKey, snapFields, snapComponents,
                                                               snapIssueTypeMeta, snapErr)) {
                AvailableFields = std::move(snapFields);
                AvailableComponents = std::move(snapComponents);
                AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
                fieldCatalogEverLoaded_ = true;
                LastTrackerFieldCatalogError.clear();
                LastTrackerFieldCatalogWarning =
                    std::string("Offline: restored ") + (catalogPlane ? "Plane" : "Jira") +
                    " field catalog from local snapshot. Last fetch failed: " + error;
                if (!catalogPlane) {
                    for (auto& field : AvailableFields) {
                        if (field.Id == "comment" || IsNonEditableTimetrackingFieldId(field.Id)) {
                            field.ReadOnly = true;
                        }
                    }
                    EnsureCatalogHistoryField();
                }
                TrackerFieldCatalogRevision.fetch_add(1);
                LOG_WARN("AppController::SetFieldCatalog transport failure; loaded snapshot err=%s", snapErr.c_str());
                return;
            }

            if (fieldCatalogEverLoaded_) {
                LastTrackerFieldCatalogError.clear();
                LastTrackerFieldCatalogWarning =
                    "Offline: no field catalog snapshot could be loaded for this tracker context. Last fetch failed: " +
                    error;
                TrackerFieldCatalogRevision.fetch_add(1);
                LOG_WARN("AppController::SetFieldCatalog transport failure; no snapshot (session had catalog): %s",
                         error.c_str());
                return;
            }

            AvailableFields.clear();
            AvailableComponents.clear();
            AvailableIssueTypeMeta.clear();
            fieldCatalogEverLoaded_ = false;
            LastTrackerFieldCatalogWarning.clear();
            LastTrackerFieldCatalogError = std::string("No cached ") + (catalogPlane ? "Plane" : "Jira") +
                                        " field catalog available. " +
                                        (error.empty() ? std::string("Last fetch failed.") : error);
            TrackerFieldCatalogRevision.fetch_add(1);
            LOG_ERROR("AppController::SetFieldCatalog error (no cache): %s", error.c_str());
            return;
        }

        AvailableFields.clear();
        AvailableComponents.clear();
        AvailableIssueTypeMeta.clear();
        fieldCatalogEverLoaded_ = false;
        LastTrackerFieldCatalogWarning.clear();
        LastTrackerFieldCatalogError = error;
        TrackerFieldCatalogRevision.fetch_add(1);
        LOG_ERROR("AppController::SetFieldCatalog error: %s", error.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(availableFieldsMutex_);
        AvailableFields = std::move(fields);
        AvailableComponents = std::move(components);
        AvailableIssueTypeMeta = std::move(issueTypeMeta);
    }
    LastTrackerFieldCatalogError.clear();
    LastTrackerFieldCatalogWarning.clear();
    fieldCatalogEverLoaded_ = true;
    requestDeferredLiveTrackerBackendSuccessNotify_();
    {
        std::string snapErr;
        if (!FieldCatalogCache::SaveFieldCatalogSnapshot(catalogCacheKey, AvailableFields, AvailableComponents,
                                                         AvailableIssueTypeMeta, snapErr)) {
            LOG_WARN("AppController::SetFieldCatalog: snapshot save failed: %s", snapErr.c_str());
        }
    }
    if (!catalogPlane) {
        for (auto& field : AvailableFields) {
            if (field.Id == "comment" || IsNonEditableTimetrackingFieldId(field.Id)) {
                field.ReadOnly = true;
            }
        }
        EnsureCatalogHistoryField();
    }

    TrackerFieldCatalogRevision.fetch_add(1);
}

const TrackerField* AppController::FindFieldById(const std::string& fieldId) const {
    std::lock_guard<std::mutex> lk(availableFieldsMutex_);
    const auto it = std::find_if(AvailableFields.begin(), AvailableFields.end(),
                                 [&](const TrackerField& field) { return field.Id == fieldId; });
    return it == AvailableFields.end() ? nullptr : &(*it);
}

void AppController::EnsureCatalogHistoryField() {
    if (FindFieldById("history") != nullptr) {
        return;
    }
    TrackerField historyField;
    historyField.Id = "history";
    historyField.Name = "History";
    historyField.ReadOnly = true;
    AvailableFields.push_back(std::move(historyField));
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
    (void)originalEstimateSnapshot;
    (void)remainingEstimateSnapshot;
    outError.clear();
    outDisplayValues.clear();
    outFieldsPayload = nlohmann::json::object();
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }
    if (!Backend) {
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
    if (!Backend->BuildFieldPayload(field, rawValues, valuePayload, outError)) {
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
            displayValue += Backend->ResolveDisplayValue(field.Id, &field, values[i]);
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
    if (!Backend) {
        return;
    }
    const auto ticketsSnap = GetActiveTicketsSnapshot();
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
    if (representatives.empty()) {
        return;
    }
    LaunchBackgroundTask([this, representatives, trackerCfgForWorker = std::move(trackerCfgForWorker)]() mutable {
        for (const auto& pair : representatives) {
            if (shuttingDown_.load()) {
                break;
            }
            std::string ignored;
            EnsureIssueEditMetaLoaded(pair.second, &ignored, nullptr, &trackerCfgForWorker);
        }
    });
}

bool AppController::CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                                         const TrackerField* fieldMeta, const std::string* issueTypeKeyOverride) const {
    if (!Backend || issueId.empty() || fieldId.empty()) {
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
                    // Jira often omits `priority` from editmeta (e.g. Epic) while PUT still accepts it; see
                    // AppController::CanEditFieldForIssue doc comment.
                    if (fieldKey == "priority") {
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
        if (fieldKey == "priority") {
            return true;
        }
        return false;
    }
    return fieldIt->second;
}

bool AppController::EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError,
                                              const std::string* issueTypeKeyOverride,
                                              const TrackerConfig* configSnapshot) {
    if (outError) {
        outError->clear();
    }
    if (!Backend || issueId.empty()) {
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
    const bool ok = Backend->FetchIssueEditMeta(cfg, issueId, meta, fetchError);

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
    if (!Backend || issueId.empty()) {
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

bool AppController::SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues, std::string& outError) {
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::SubmitFieldEdit blocked by read-only mode issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }
    if (!Backend || !Cache) {
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

    ITrackerClient& backend = *Backend;

    const std::string fieldEditAuditOp = BackendAuditTrail::MakeOperationId("field-edit");
    const char* const fieldEditAuditSource = FieldEditAuditSource::Current();
    LOG_TRACE("SubmitFieldEdit: source=%s issue=%s field=%s raw_values=%zu", fieldEditAuditSource, issueId.c_str(),
              field.Id.c_str(), rawValues.size());

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    const std::shared_ptr<const std::vector<CachedTicket>> ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;

    if (IsSprintField(field)) {
        if (values.empty()) {
            outError = "Clearing sprint is not supported by this action.";
            LOG_WARN("AppController::SubmitFieldEdit sprint clear not supported issue=%s field=%s", issueId.c_str(),
                     field.Id.c_str());
            return false;
        }
        const std::string sprintId = values.front();
        auto ticketIt = std::find_if(tickets.begin(), tickets.end(),
                                     [&](const CachedTicket& ticket) { return ticket.id == issueId; });
        BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                       nlohmann::json{{"field_id", field.Id}, {"kind", "sprint"}});
        if (!backend.AddIssueToSprint(issueId, sprintId, outError)) {
            LOG_ERROR("AppController::SubmitFieldEdit sprint update failed issue=%s field=%s sprint=%s err=%s",
                      issueId.c_str(), field.Id.c_str(), sprintId.c_str(), outError.c_str());
            BackendAuditTrail::AppendResult(
                "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
                nlohmann::json{
                    {"field_id", field.Id},
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

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outError = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        LOG_WARN("AppController::SubmitFieldEdit blocked non-editable timetracking issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        const std::string editedValue = values.empty() ? std::string() : values.front();
        if (editedValue.empty()) {
            outError = "Clearing Jira timetracking estimates is not supported by this editor.";
            LOG_WARN("AppController::SubmitFieldEdit blocked timetracking clear issue=%s field=%s", issueId.c_str(),
                     field.Id.c_str());
            return false;
        }

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
        if (!backend.UpdateIssueFields(issueId, fieldsPayload, outError)) {
            std::string payloadForLog;
            try {
                payloadForLog = fieldsPayload.dump();
            } catch (...) {
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

    if (!IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id)) {
        EnsureIssueEditMetaLoaded(issueId, nullptr);
    }

    if (!IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id) &&
        !CanEditFieldForIssue(issueId, field.Id, &field)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        LOG_WARN("AppController::SubmitFieldEdit blocked by editmeta issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    nlohmann::json fieldsPayload;
    if (!backend.BuildFieldPayload(field, rawValues, fieldsPayload, outError)) {
        LOG_WARN("AppController::SubmitFieldEdit invalid value issue=%s field=%s err=%s", 
                 issueId.c_str(), field.Id.c_str(), outError.c_str());
        return false;
    }

    BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", field.Id}, {"kind", "issue_fields"}});
    bool updateOk = backend.UpdateIssueFields(issueId, fieldsPayload, outError);
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
        updateOk = backend.UpdateIssueFields(issueId, fieldsPayload, outError);
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
                displayValue += backend.ResolveDisplayValue(field.Id, &field, values[i]);
            }
        }

        updatedTicket.fieldValues[field.Id] = displayValue;
        UpdateTicket(updatedTicket);
        BackendAuditTrail::AppendResult("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
                                        nlohmann::json{{"field_id", field.Id},
                                                       {"before", ticketIt->GetFieldValue(field.Id)},
                                                       {"after", displayValue}});
    } else {
        RefreshLocalData();
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{{"field_id", field.Id}, {"before", "unknown"}, {"after", rawValues}});
    }

    requestDeferredLiveTrackerBackendSuccessNotify_();
    return true;
}

bool AppController::SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues,
                                               const std::string& originalEstimateSnapshot,
                                               const std::string& remainingEstimateSnapshot,
                                               const std::string& issueTypeKeySnapshot, FieldEditResult& outResult) {
    outResult = FieldEditResult{};
    LOG_TRACE("SubmitFieldEditNetworkOnly: source=%s issue=%s field=%s raw_values=%zu",
              FieldEditAuditSource::Current(), issueId.c_str(), field.Id.c_str(), rawValues.size());
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

    if (!Backend) {
        outResult.Error = "No tracker backend initialized.";
        return false;
    }
    ITrackerClient& backend = *Backend;
    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    if (IsSprintField(field)) {
        if (values.empty()) {
            outResult.Error = "Clearing sprint is not supported by this action.";
            return false;
        }
        const std::string sprintId = values.front();
        if (!backend.AddIssueToSprint(issueId, sprintId, outResult.Error)) {
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

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outResult.Error = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        return false;
    }

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
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
        if (!backend.UpdateIssueFields(issueId, fieldsPayload, outResult.Error)) {
            return false;
        }
        outResult.Ok = true;
        outResult.UpdatedDisplayValues["timeoriginalestimate"] = std::move(originalEstimate);
        outResult.UpdatedDisplayValues["timeestimate"] = std::move(remainingEstimate);
        requestDeferredLiveTrackerBackendSuccessNotify_();
        return true;
    }

    const std::string* issueTypeKeyOpt = issueTypeKeySnapshot.empty() ? nullptr : &issueTypeKeySnapshot;

    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(issueId, field, rawValues, originalEstimateSnapshot,
                                            remainingEstimateSnapshot, issueTypeKeySnapshot, fieldsPayload,
                                            displayValues, outResult.Error)) {
        return false;
    }

    bool updateOk = backend.UpdateIssueFields(issueId, fieldsPayload, outResult.Error);
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
        updateOk = backend.UpdateIssueFields(issueId, fieldsPayload, outResult.Error);
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

    outResult.Ok = true;
    outResult.UpdatedDisplayValues = std::move(displayValues);
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
    outWatchers.clear();
    outError.clear();
    if (!Backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const bool ok = Backend->FetchIssueWatchers(cfg, issueKey, outWatchers, outError);
    if (!ok) {
        LOG_ERROR("AppController::FetchIssueWatchers failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::FetchIssueVotes(const std::string& issueKey, std::vector<TrackerUser>& outVoters,
                                    std::string& outError, int* outVoteCount, bool* outHasVoted,
                                    bool* outVotersInResponse) const {
    outVoters.clear();
    outError.clear();
    if (!Backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const bool ok = Backend->FetchIssueVotes(cfg, issueKey, outVoters, outError, outVoteCount, outHasVoted, outVotersInResponse);
    if (!ok) {
        LOG_ERROR("AppController::FetchIssueVotes failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::SearchUsersByQuery(const std::string& query, std::vector<TrackerUser>& outUsers,
                                           std::string& outError) const {
    outUsers.clear();
    outError.clear();
    if (!Backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const bool ok = Backend->SearchUsersByQuery(cfg, query, outUsers, outError);
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
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::AddIssueCommentPlain blocked by read-only mode issue=%s", issueKey.c_str());
        return false;
    }
    if (!Backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const bool ok = Backend->AddIssueCommentPlain(cfg, issueKey, plainText, outError);
    if (!ok) {
        LOG_ERROR("AppController::AddIssueCommentPlain failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::SubmitWorklog(const std::string& issueId, const std::string& timeSpent,
                                   const std::string& timeRemaining, const std::string& adjustEstimate,
                                   const std::string& workDescription, const std::string& startedDate,
                                   std::string& outError) {
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::SubmitWorklog blocked by read-only mode issue=%s", issueId.c_str());
        return false;
    }
    if (!Backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const bool ok = Backend->AddWorklog(cfg, issueId, timeSpent, timeRemaining, adjustEstimate, workDescription, startedDate, outError);
    if (!ok) {
        LOG_ERROR("AppController::SubmitWorklog failed issue=%s err=%s", issueId.c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
        PrefetchIssueTicketsForKeys({issueId}, true);
    }
    return ok;
}

bool AppController::AddIssueCommentBlameContext(const std::string& issueKey, const std::string& p4User,
                                                    const std::string& functionName, const std::string& filePath,
                                                    const int lineNumber, const std::string& changelist,
                                                    const std::string& date, const bool approximated,
                                                    const std::string& codeSnippet, std::string& outError) {
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("AppController::AddIssueCommentBlameContext blocked by read-only mode issue=%s", issueKey.c_str());
        return false;
    }
    if (!Backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const bool ok = Backend->AddIssueCommentBlameContext(cfg, issueKey, p4User, functionName, filePath, lineNumber,
                                                             changelist, date, approximated, codeSnippet, outError);
    if (!ok) {
        LOG_ERROR("AppController::AddIssueCommentBlameContext failed issue=%s err=%s", issueKey.c_str(),
                  outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::FetchUserGroupNames(const std::string& accountId, std::vector<std::string>& outGroupNames,
                                            std::string& outError) const {
    outGroupNames.clear();
    outError.clear();
    if (!Backend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    const bool ok = Backend->FetchUserGroupNames(cfg, accountId, outGroupNames, outError);
    if (!ok) {
        LOG_ERROR("AppController::FetchUserGroupNames failed account=%s err=%s",
                  TruncateForLog(accountId, 40).c_str(), outError.c_str());
    } else {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    return ok;
}






