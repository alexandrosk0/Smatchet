#include "PlaneClient.h"
#include "PlaneClient_Internal.h"
#include "PlaneCommentMappingPure.h"
#include "PlaneCustomPropertyPure.h"
#include "PlaneIssueMappingPure.h"

#include "IssueDraft.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "MarkdownConvert.h"
#include "StringUtil.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cpr/cpr.h>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

using smatchet::plane_detail::JsonFieldToString;
using smatchet::plane_detail::NormalizePlaneApiBase;
using smatchet::plane_detail::ResolvePlaneProject;

namespace {

bool LooksLikeUuid(const std::string& s) {
    if (s.size() != 36)
        return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0)
            return false;
    }
    return true;
}

} // namespace

void PlaneClient::SetMutationCancelToken(std::shared_ptr<std::atomic<bool>> token) {
    // Single-writer (AppController, before any worker spawns / during backend create) — plain store.
    mutationCancel_ = std::move(token);
}

TrackerError PlaneClient::UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) {
    std::string outError;
    // Observe the shutdown/abort token (if installed) on every retry attempt + backoff of the
    // blocking PATCH below, so an automation worker blocked here unwinds promptly when shutdown raises
    // it (mutation-path twin of the search-path cancel plumbing, #1529). Captured by value: the
    // shared_ptr keeps the token alive for the call even across a concurrent backend swap.
    std::shared_ptr<std::atomic<bool>> cancelTok = mutationCancel_;
    std::function<bool()> cancelled =
        cancelTok ? std::function<bool()>([cancelTok]() { return cancelTok->load(); }) : std::function<bool()>();
    // Resolve config + headers + project + UUID under the cache lock, then drop the lock before
    // the HTTP PATCH so UI thread calls to ResolveDisplayValue / display-name lookups are not
    // blocked for the duration of the round trip.
    TrackerConfig cfg = ConfigManager::Load();
    // No global cfg.PlaneProjectId. Resolve via the active view query (filled by the
    // current view; legacy installs swept by the one-shot startup migration). UpdateIssueFields is invoked from grid
    // edits which are scoped to the current view → its query carries the project.
    const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);
    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    std::string resolvedProjectId;
    std::string targetUuid;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        TrackerError resolveClassified;
        if (planeProjectId_.empty()) {
            // First-call HTTP under the lock is acceptable: it runs at most once per process
            // lifetime; the steady-state PATCH path below is the hot one that needed the fix.
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, planeProjectId_, planeProjectIdentifier_, &outError,
                                &resolveClassified);
        }
        if (planeProjectId_.empty()) {
            // Classified at the resolve failure site (N12 slice 3) — a transport failure stays
            // retryable so the offline-queue replay path keeps working.
            return resolveClassified.IsOk() ? TrackerErrorUnknown(outError) : resolveClassified;
        }
        resolvedProjectId = planeProjectId_;

        targetUuid = issueId;
        if (!LooksLikeUuid(issueId)) {
            auto it = keyToId_.find(issueId);
            if (it != keyToId_.end()) {
                targetUuid = it->second;
            } else {
                // If not in cache, we might be replaying an offline edit after restart.
                // In a real app we might need to search Plane for this key to get the UUID,
                // but for now we'll assume it's in the cache from a recent fetch.
                outError = "Could not resolve Plane visual key '" + issueId + "' to UUID. Try refreshing the grid.";
                return TrackerErrorInvalidRequest(outError);
            }
        }
    }

    const std::string url = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                            resolvedProjectId + "/work-items/" + targetUuid + "/";

    // §B2 decision (2026-07-05): this stays a SINGLE HTTP attempt — do NOT wrap in
    // TrackerHttpRequestWithRetry. UpdateIssueFields is driven by the offline-queue replay loop
    // (OfflineQueueService::ReplayOneFieldEdit), which already retries transient failures on its
    // own tick with attempt bookkeeping; adding per-call retry here would stack two retry loops
    // and block the replay worker for the internal backoff. Direct (online) callers accept a
    // single attempt and surface a retryable TrackerError to the user.
    auto response = TrackerPatchLogged("PlaneClient", url, headers, fields.dump(), cancelled);
    LogTrackerHttpResult("PlaneClient", "PATCH", url, response);

    if (response.status_code != 200 && response.status_code != 204) {
        std::string detail;
        try {
            std::string parseErr;
            auto j = smatchet::json_safe::ParseBounded(response.text, parseErr);
            if (!parseErr.empty()) {
                detail = response.text;
            } else if (j.is_object() && j.contains("error")) {
                detail = j["error"].dump();
            } else if (j.is_object() && j.contains("detail")) {
                detail = j["detail"].dump();
            } else {
                detail = response.text;
            }
        } catch (...) { // catch-all-ok: error body not JSON — fall back to the raw response text
            detail = response.text;
        }
        outError = "Plane API error: " + std::to_string(response.status_code) + " " + RedactHttpBodyForLog(detail);
        // 2xx-but-not-200/204 (e.g. 206) reaches this failure branch; guard before FromHttpStatus,
        // which would map a 2xx to Ok() and silently drop the detail (plan FIX-1 / Slice-2 precedent).
        if (response.status_code >= 200 && response.status_code < 300) {
            return TrackerErrorUnknown(outError, response.status_code);
        }
        return TrackerErrorFromHttpStatus(response.status_code, outError);
    }
    return TrackerError::Ok();
}

Result<bool, TrackerError> PlaneClient::ProbeIssueExists(const TrackerConfig& cfg, const std::string& issueKey) {
    // SMATCHET_DEVIATION(rule=duplication; reason=backend-parity probe; owner=tracker-backend; revisit=2026-12-31)
    using ProbeResult = Result<bool, TrackerError>;
    // ticket-change-monitor: one work-item GET to tell a deletion (404 → Ok(false)) apart from an
    // issue that merely left the view (still 200 → Ok(true)). Mirrors UpdateIssueFields' project +
    // visual-key→UUID resolution. Any other non-200 is an inconclusive Err the reconcile treats
    // non-destructively (keeps the shared cache row).
    if (issueKey.empty()) {
        return ProbeResult::Err(TrackerErrorInvalidRequest("ProbeIssueExists: empty issue key"));
    }
    std::string outError;
    const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);
    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    std::string resolvedProjectId;
    std::string targetUuid;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        TrackerError resolveClassified;
        if (planeProjectId_.empty()) {
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, planeProjectId_, planeProjectIdentifier_, &outError,
                                &resolveClassified);
        }
        if (planeProjectId_.empty()) {
            return ProbeResult::Err(resolveClassified.IsOk() ? TrackerErrorUnknown(outError) : resolveClassified);
        }
        resolvedProjectId = planeProjectId_;

        targetUuid = issueKey;
        if (!LooksLikeUuid(issueKey)) {
            auto it = keyToId_.find(issueKey);
            if (it == keyToId_.end()) {
                // Not in the visual-key→UUID cache (never fetched, or evicted). Cannot form the GET
                // URL — return non-destructive "still exists" so the reconcile keeps the row.
                LOG_INFO("PlaneClient::ProbeIssueExists: no cached UUID for '%s'; treating as still-exists",
                         issueKey.c_str());
                return ProbeResult::Ok(true);
            }
            targetUuid = it->second;
        }
    }

    const std::string url = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                            resolvedProjectId + "/work-items/" + targetUuid + "/";
    // SMATCHET_DEVIATION(rule=duplication; reason=backend-parity classify; owner=tracker-backend; revisit=2026-12-31)
    const cpr::Response resp = TrackerGetLogged("PlaneClient", url, headers);
    if (resp.status_code == 200) {
        return ProbeResult::Ok(true);
    }
    if (resp.status_code == 404) {
        return ProbeResult::Ok(false);
    }
    if (resp.status_code >= 200 && resp.status_code < 300) {
        return ProbeResult::Err(
            TrackerErrorUnknown("ProbeIssueExists: unexpected 2xx", static_cast<int>(resp.status_code)));
    }
    return ProbeResult::Err(
        TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), "ProbeIssueExists HTTP error"));
}

Result<nlohmann::json, TrackerError> PlaneClient::BuildFieldPayload(const TrackerField& field,
                                                                    const std::vector<std::string>& values) {
    std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
    nlohmann::json outPayload = nlohmann::json::object();
    if (field.Id == "summary") {
        outPayload["name"] = values.empty() ? "" : values[0];
    } else if (field.Id == "description") {
        // Modal editor produces Markdown; convert to the HTML subset Plane accepts. Empty input
        // round-trips to empty string. The modal's raw-mode save (kicks in for HTML the converter
        // can't safely round-trip) ships verbatim — sniffed by "starts with `<` and contains
        // a closing tag", which is essentially never true for real user-typed Markdown. See
        // docs/plans/rich-text-editing-v2-remaining.md.
        const std::string& md = values.empty() ? std::string() : values[0];
        if (md.empty()) {
            outPayload["description_html"] = std::string();
        } else {
            size_t firstNonWs = 0;
            while (firstNonWs < md.size() && std::isspace(static_cast<unsigned char>(md[firstNonWs])))
                ++firstNonWs;
            const bool looksLikeRawHtml =
                firstNonWs < md.size() && md[firstNonWs] == '<' && md.find("</") != std::string::npos;
            outPayload["description_html"] = looksLikeRawHtml ? md : MarkdownConvert::MarkdownToHtml(md);
        }
    } else if (field.Id == "priority") {
        outPayload["priority"] = values.empty() ? "medium" : values[0];
    } else if (field.Id == "status") {
        outPayload["state"] = values.empty() ? "" : values[0];
    } else if (field.Id == "assignee") {
        outPayload["assignee"] = values.empty() ? nullptr : nlohmann::json(values[0]);
    } else if (field.Id == "labels") {
        std::vector<std::string> labelIds;
        for (const auto& val : values) {
            auto lIt =
                std::find_if(cachedLabels_.begin(), cachedLabels_.end(), [&](const auto& l) { return l.Name == val; });
            std::string id = (lIt != cachedLabels_.end()) ? lIt->Id : val;
            labelIds.push_back(id);
        }
        outPayload["labels"] = labelIds;
    } else if (field.Id == "sprint") {
        outPayload["cycle"] = values.empty() ? nullptr : nlohmann::json(values[0]);
    } else {
        return Result<nlohmann::json, TrackerError>::Err(
            TrackerErrorInvalidRequest("Field not supported for update in Plane: " + field.Id));
    }
    return Result<nlohmann::json, TrackerError>::Ok(std::move(outPayload));
}

TrackerError PlaneClient::UpdateField(const std::string& issueId, const TrackerField& field,
                                      const std::vector<std::string>& values) {
    auto payloadResult = BuildFieldPayload(field, values);
    if (!payloadResult) {
        return payloadResult.error();
    }
    return UpdateIssueFields(issueId, payloadResult.value());
}

std::string PlaneClient::ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                             const std::string& value) const {
    std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
    if (fieldId == "status") {
        auto sIt =
            std::find_if(cachedStates_.begin(), cachedStates_.end(), [&](const auto& s) { return s.Id == value; });
        if (sIt != cachedStates_.end())
            return sIt->Name;
    }

    if (!field) {
        return value;
    }
    if (field->Id == "sprint") {
        auto cIt =
            std::find_if(cachedCycles_.begin(), cachedCycles_.end(), [&](const auto& c) { return c.Id == value; });
        if (cIt != cachedCycles_.end())
            return cIt->Name;
    }
    if (field->Id == "issuetype") {
        auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                  [&](const auto& opt) { return opt.Id == value; });
        if (optIt != field->AllowedValueOptions.end())
            return optIt->Value;
    }
    if (field->Id == "assignee" || field->IsUserType) {
        if (value.empty()) {
            return value;
        }
        auto uIt =
            std::find_if(cachedUsers_.begin(), cachedUsers_.end(), [&](const auto& u) { return u.AccountId == value; });
        if (uIt != cachedUsers_.end())
            return uIt->DisplayName;
        auto uNameIt = std::find_if(cachedUsers_.begin(), cachedUsers_.end(),
                                    [&](const auto& u) { return u.DisplayName == value; });
        if (uNameIt != cachedUsers_.end())
            return uNameIt->DisplayName;
        // Fallback to searching AllowedValueOptions if not in cachedUsers_
        auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                  [&](const auto& opt) { return opt.Id == value; });
        if (optIt != field->AllowedValueOptions.end())
            return optIt->Value;
        auto optNameIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                      [&](const auto& opt) { return opt.Value == value; });
        if (optNameIt != field->AllowedValueOptions.end())
            return optNameIt->Value;
        LOG_DEBUG("PlaneClient: Failed to resolve user UUID '%s' for field '%s' (cache size: %zu)", value.c_str(),
                  field->Id.c_str(), cachedUsers_.size());
    }
    if (field->Id == "labels") {
        // Multi-select labels might be comma-separated or single UUID
        if (value.find(',') != std::string::npos) {
            std::vector<std::string> parts = SplitAndTrim(value);
            std::string resolved;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0)
                    resolved += ", ";
                auto lIt = std::find_if(cachedLabels_.begin(), cachedLabels_.end(),
                                        [&](const auto& l) { return l.Id == parts[i]; });
                if (lIt != cachedLabels_.end()) {
                    resolved += lIt->Name;
                } else {
                    auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                              [&](const auto& opt) { return opt.Id == parts[i]; });
                    if (optIt != field->AllowedValueOptions.end()) {
                        resolved += optIt->Value;
                    } else {
                        resolved += parts[i];
                    }
                }
            }
            return resolved;
        } else {
            auto lIt =
                std::find_if(cachedLabels_.begin(), cachedLabels_.end(), [&](const auto& l) { return l.Id == value; });
            if (lIt != cachedLabels_.end())
                return lIt->Name;

            auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                      [&](const auto& opt) { return opt.Id == value; });
            if (optIt != field->AllowedValueOptions.end())
                return optIt->Value;
        }
    }

    return value;
}

Result<std::string, TrackerError> PlaneClient::CreateIssue(const nlohmann::json& fields) {
    std::string outError;
    // Resolve config + headers + project under the cache lock, then drop the lock before the HTTP
    // POST so UI display-name lookups are not blocked during the round trip. Re-acquire briefly at
    // the end to record `visualKey -> uuid` in `keyToId_`.
    // Backlog #12: the cfg snapshot and the cache read must happen under the same lock.
    // Snapshotting cfg before the lock would let a concurrent ConfigManager::Save() rotate
    // credentials or workspace identifiers between the snapshot and the cache lookup — the
    // request would then go out with stale auth while the cache was inspected with fresh state.
    std::string planeApi;
    cpr::Header headers;
    std::string workspaceSlug;
    std::string resolvedProjectId;
    std::string projectIdentifier;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        TrackerConfig cfg = ConfigManager::Load();
        // No global cfg.PlaneProjectId (see remove-global-project-key.md §2.5).
        // Resolve from the active view's query — same pattern as UpdateIssueFields above.
        const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);
        planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
        for (const auto& kv : BuildPlaneHeaders(cfg)) {
            headers.insert({kv.first, kv.second});
        }
        workspaceSlug = cfg.PlaneWorkspaceSlug;
        TrackerError resolveClassified;
        if (planeProjectId_.empty()) {
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, planeProjectId_, planeProjectIdentifier_, &outError,
                                &resolveClassified);
        }
        if (planeProjectId_.empty()) {
            // Classified at the resolve failure site (N12 slice 3) so a transport failure stays retryable.
            return Result<std::string, TrackerError>::Err(resolveClassified.IsOk() ? TrackerErrorUnknown(outError)
                                                                                   : resolveClassified);
        }
        resolvedProjectId = planeProjectId_;
        projectIdentifier = planeProjectIdentifier_;
    }

    const std::string url =
        planeApi + "/api/v1/workspaces/" + workspaceSlug + "/projects/" + resolvedProjectId + "/work-items/";

    // §B2 decision (2026-07-05): SINGLE HTTP attempt — do NOT add retry. CreateIssue is a
    // non-idempotent POST: a retry after the server already committed the create (5xx / timeout
    // after receipt) would duplicate the issue. Durability/retry for queued creates is owned by
    // OfflineQueueService::ReplayOneCreate (which de-dups via the pending-create latch), not here.
    auto response = TrackerPostLogged("PlaneClient", url, headers, fields.dump());
    LogTrackerHttpResult("PlaneClient", "POST", url, response);

    if (response.status_code != 200 && response.status_code != 201) {
        std::string detail;
        try {
            std::string parseErr;
            auto j = smatchet::json_safe::ParseBounded(response.text, parseErr);
            // Plane often returns {"error": "..."} or {"detail": "..."} or a dict of field errors
            if (!parseErr.empty()) {
                detail = response.text;
            } else if (j.is_object()) {
                if (j.contains("error"))
                    detail = j["error"].dump();
                else if (j.contains("detail"))
                    detail = j["detail"].dump();
                else
                    detail = j.dump(); // Show the full object if it's field-level errors
            } else {
                detail = response.text;
            }
        } catch (...) { // catch-all-ok: error body not JSON — fall back to the raw response text
            detail = response.text;
        }
        outError = "Plane API error: " + std::to_string(response.status_code) + " " + RedactHttpBodyForLog(detail);
        // 2xx-but-not-200/201 reaches this failure branch; guard before FromHttpStatus (FIX-1 / Slice-2).
        if (response.status_code >= 200 && response.status_code < 300) {
            return Result<std::string, TrackerError>::Err(TrackerErrorUnknown(outError, response.status_code));
        }
        return Result<std::string, TrackerError>::Err(TrackerErrorFromHttpStatus(response.status_code, outError));
    }

    try {
        std::string parseErr;
        auto j = smatchet::json_safe::ParseBounded(response.text, parseErr);
        if (!parseErr.empty()) {
            // Network/API tier (exception-handling-policy.md): the create succeeded server-side
            // (2xx above) but its response body did not parse — surface it instead of swallowing,
            // then fall through to an empty key so the caller treats it as "created, key unknown".
            LOG_WARN("PlaneClient::CreateIssue: issue created but response JSON failed to parse: %s", parseErr.c_str());
        } else {
            // SMATCHET_DEVIATION(rule=duplication; reason=ParseBounded clone #9; owner=cpp-audit; revisit=2026-09-30)
            const std::string uuid = JsonFieldToString(j, "id");
            const std::string seqId = JsonFieldToString(j, "sequence_id");

            std::string visualKey;
            if (!projectIdentifier.empty() && !seqId.empty()) {
                visualKey = projectIdentifier + "-" + seqId;
            } else if (!seqId.empty()) {
                visualKey = "#" + seqId;
            } else {
                visualKey = uuid;
            }

            if (!uuid.empty() && !visualKey.empty()) {
                std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
                keyToId_[visualKey] = uuid;
            }

            return Result<std::string, TrackerError>::Ok(visualKey);
        }
    } catch (const std::exception& ex) {
        // Network/API tier (exception-handling-policy.md): the create succeeded server-side
        // (2xx above) but its response body did not parse — surface it instead of swallowing,
        // then fall through to an empty key so the caller treats it as "created, key unknown".
        LOG_WARN("PlaneClient::CreateIssue: issue created but response JSON failed to parse: %s", ex.what());
    } catch (...) {
        LOG_WARN("PlaneClient::CreateIssue: issue created but response JSON failed to parse: unknown exception");
    }

    // "Created, key unknown" — preserve the prior empty-key-no-error contract: Ok(empty) (NOT Err),
    // so the caller's existing empty-key check reports "Create failed." exactly as before.
    return Result<std::string, TrackerError>::Ok(std::string());
}

Result<std::vector<std::pair<std::string, std::string>>, TrackerError>
PlaneClient::AttachFilesToIssue(const std::string& /*issueKey*/, const std::vector<std::string>& /*absolutePaths*/) {
    return Result<std::vector<std::pair<std::string, std::string>>, TrackerError>::Err(
        TrackerErrorInvalidRequest("AttachFilesToIssue not implemented for Plane"));
}

TrackerError PlaneClient::AddIssueToSprint(const std::string& issueKey, const std::string& sprintId) {
    nlohmann::json payload;
    payload["cycle"] = sprintId;
    return UpdateIssueFields(issueKey, payload);
}

Result<nlohmann::json, TrackerError> PlaneClient::BuildCreatePayload(const IssueDraft& draft,
                                                                     const std::vector<TrackerField>& catalog) {
    nlohmann::json outPayload = nlohmann::json::object();

    outPayload["name"] = draft.FieldValues.count("summary") ? draft.FieldValues.at("summary") : "";

    if (draft.FieldValues.count("description")) {
        const std::string& desc = draft.FieldValues.at("description");
        if (!desc.empty()) {
            // New-issue draft also goes through the modal-style Markdown surface; convert to the
            // HTML subset Plane accepts. See docs/plans/rich-text-editing-v2-remaining.md.
            outPayload["description_html"] = MarkdownConvert::MarkdownToHtml(desc);
        }
    }

    outPayload["priority"] = draft.FieldValues.count("priority") ? draft.FieldValues.at("priority") : "medium";

    if (draft.FieldValues.count("status")) {
        outPayload["state"] = draft.FieldValues.at("status");
    }

    if (!draft.IssueTypeId.empty()) {
        outPayload["type"] = draft.IssueTypeId;
    } else if (!draft.IssueTypeName.empty()) {
        outPayload["type"] = draft.IssueTypeName; // Plane usually uses UUID for type
    }

    if (!draft.ParentKey.empty()) {
        outPayload["parent"] = draft.ParentKey;
    }

    if (draft.FieldValues.count("assignee")) {
        outPayload["assignee"] = draft.FieldValues.at("assignee");
    }

    // C4: custom (UUID) properties used to be silently dropped here. A validation
    // failure aborts the build — better a visible error than quiet data loss.
    auto customProps = smatchet::plane::BuildPlaneCustomProperties(draft.FieldValues, catalog);
    if (!customProps.has_value()) {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(customProps.error()));
    }
    if (!customProps.value().empty()) {
        outPayload["properties"] = std::move(customProps.value());
    }

    return Result<nlohmann::json, TrackerError>::Ok(std::move(outPayload));
}

Result<nlohmann::json, TrackerError> PlaneClient::BuildUpdatePayload(const IssueDraft& draft,
                                                                     const std::vector<TrackerField>& catalog) {
    // For Plane, update payload is the same as create payload (subset of fields)
    return BuildCreatePayload(draft, catalog);
}

namespace {

// Resolve the comments collection URL for `issueKey` from a settled cfg + pre-built
// headers. Mirrors the project + work-item-UUID resolution in PlaneClient::UpdateIssueFields.
// On success writes `outCommentsUrl` and returns Ok(); on failure returns an Err carrying
// the resolution problem. The PlaneClient state (project key→query lookup, project-id cache,
// key→UUID map, cache mutex) is threaded through explicitly so this stays a free helper.
TrackerError ResolvePlaneCommentsUrl(PlaneClient& client, const TrackerConfig& cfg, const std::string& issueKey,
                                     const cpr::Header& headers, std::string& cachedProjectId,
                                     std::string& cachedProjectIdentifier,
                                     std::unordered_map<std::string, std::string>& keyToId,
                                     std::recursive_mutex& cacheMutex, std::string& outCommentsUrl) {
    const std::string projectKey = client.ExtractProjectFromQuery(cfg.JqlQuery);
    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);

    std::string resolvedProjectId;
    std::string targetUuid;
    std::string resolveError;
    {
        std::lock_guard<std::recursive_mutex> lock(cacheMutex);
        TrackerError resolveClassified;
        if (cachedProjectId.empty()) {
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, cachedProjectId, cachedProjectIdentifier,
                                &resolveError, &resolveClassified);
        }
        if (cachedProjectId.empty()) {
            // Classified at the resolve failure site (N12 slice 3).
            return resolveClassified.IsOk() ? TrackerErrorUnknown(resolveError) : resolveClassified;
        }
        resolvedProjectId = cachedProjectId;

        targetUuid = issueKey;
        if (!LooksLikeUuid(issueKey)) {
            auto it = keyToId.find(issueKey);
            if (it != keyToId.end()) {
                targetUuid = it->second;
            } else {
                return TrackerErrorInvalidRequest("Could not resolve Plane visual key '" + issueKey +
                                                  "' to UUID. Try refreshing the grid.");
            }
        }
    }

    outCommentsUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + resolvedProjectId +
                     "/work-items/" + targetUuid + "/comments/";
    return TrackerError::Ok();
}

} // namespace

Result<std::vector<TrackerIssueComment>, TrackerError> PlaneClient::FetchIssueComments(const std::string& issueKey) {
    using CommentsResult = Result<std::vector<TrackerIssueComment>, TrackerError>;
    // No cfg parameter on this interface — resolve from the settled on-disk config
    // (same per-request pattern UpdateIssueFields / CreateIssue use).
    TrackerConfig cfg = ConfigManager::Load();
    if (cfg.PlaneUrl.empty() || cfg.PlaneApiKey.empty() || cfg.PlaneWorkspaceSlug.empty()) {
        return CommentsResult::Err(TrackerErrorAuth("Missing Plane URL, API key, or workspace slug."));
    }

    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }
    std::string commentsUrl;
    const TrackerError resolved =
        ResolvePlaneCommentsUrl(*this, cfg, issueKey, headers, planeProjectId_, planeProjectIdentifier_, keyToId_,
                                planeCacheMutex_, commentsUrl);
    if (!resolved.IsOk()) {
        return CommentsResult::Err(resolved);
    }

    std::vector<TrackerIssueComment> mapped;
    // SMATCHET_DEVIATION(rule=duplication; reason=DR25 Plane cursor-pagination loop is the uniform tracker idiom
    // (mirrors PlaneIssueSearch/PlaneActivityFeed); the per-page bodies differ, so a shared callback helper across
    // independent fetches is not worth the coupling; owner=deep-review; revisit=2026-10-01)
    std::string cursor;
    // Follow Plane's cursor pagination so comments beyond the first page are returned (DR25).
    // The page cap bounds a cursor that never signals end-of-list.
    for (int page = 0; page < 100; ++page) {
        cpr::Parameters params;
        params.Add({"per_page", "100"});
        if (!cursor.empty()) {
            params.Add({"cursor", cursor});
        }
        const cpr::Response resp = TrackerGetLogged("PlaneClient", commentsUrl, headers, params);
        LogTrackerHttpResult("PlaneClient", "GET", commentsUrl, resp);
        if (resp.status_code != 200) {
            const std::string msg = "Plane API error: " + std::to_string(resp.status_code);
            LOG_ERROR("PlaneClient::FetchIssueComments: HTTP %ld for %s", resp.status_code, issueKey.c_str());
            if (resp.status_code >= 200 && resp.status_code < 300) {
                return CommentsResult::Err(TrackerErrorUnknown(msg, resp.status_code));
            }
            return CommentsResult::Err(TrackerErrorFromHttpStatus(resp.status_code, msg));
        }

        std::string parseErr;
        const nlohmann::json parsedJson = smatchet::json_safe::ParseBounded(StripUtf8BomCopy(resp.text), parseErr);
        if (!parseErr.empty()) {
            LOG_ERROR("PlaneClient::FetchIssueComments: invalid JSON for %s: %s", issueKey.c_str(), parseErr.c_str());
            return CommentsResult::Err(TrackerErrorParse("Plane issue-comments response was not valid JSON."));
        }
        // Plane returns either a bare array or a paginated `{ "results": [...] }` envelope.
        const nlohmann::json& nodes =
            (parsedJson.is_object() && parsedJson.contains("results")) ? parsedJson["results"] : parsedJson;
        std::vector<TrackerIssueComment> pageComments = smatchet::plane::MapPlaneIssueComments(nodes);
        mapped.insert(mapped.end(), std::make_move_iterator(pageComments.begin()),
                      std::make_move_iterator(pageComments.end()));
        cursor = smatchet::plane::NextPaginationCursor(parsedJson);
        if (cursor.empty()) {
            break;
        }
    }
    LOG_INFO("PlaneClient::FetchIssueComments: %s → %zu comments", issueKey.c_str(), mapped.size());
    return CommentsResult::Ok(std::move(mapped));
}

TrackerError PlaneClient::AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                               const std::string& plainText) {
    // cfg-carrying mutation — resolve credentials from the live cfg (matches CreateIssue /
    // UpdateIssueFields). Direct-post: comments are exempt from the offline-queue + audit-trail
    // wiring (mirrors the existing GitHub / Jira comment post paths).
    if (cfg.PlaneUrl.empty() || cfg.PlaneApiKey.empty() || cfg.PlaneWorkspaceSlug.empty()) {
        return TrackerErrorAuth("Missing Plane URL, API key, or workspace slug.");
    }

    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }
    std::string commentsUrl;
    const TrackerError resolved =
        ResolvePlaneCommentsUrl(*this, cfg, issueKey, headers, planeProjectId_, planeProjectIdentifier_, keyToId_,
                                planeCacheMutex_, commentsUrl);
    if (!resolved.IsOk()) {
        return resolved;
    }

    nlohmann::json body = nlohmann::json::object();
    // Plane stores comment bodies as HTML; convert the plain/Markdown text the modal produced
    // into the HTML subset Plane accepts (same converter the description field uses).
    body["comment_html"] = MarkdownConvert::MarkdownToHtml(plainText);

    const cpr::Response resp = TrackerPostLogged("PlaneClient", commentsUrl, headers, body.dump());
    LogTrackerHttpResult("PlaneClient", "POST", commentsUrl, resp);
    if (resp.status_code != 200 && resp.status_code != 201) {
        const std::string msg = "Plane API error: " + std::to_string(resp.status_code);
        LOG_ERROR("PlaneClient::AddIssueCommentPlain: HTTP %ld for %s", resp.status_code, issueKey.c_str());
        if (resp.status_code >= 200 && resp.status_code < 300) {
            return TrackerErrorUnknown(msg, resp.status_code);
        }
        return TrackerErrorFromHttpStatus(resp.status_code, msg);
    }
    return TrackerError::Ok();
}
