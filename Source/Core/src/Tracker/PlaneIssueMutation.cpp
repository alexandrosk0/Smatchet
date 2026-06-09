#include "PlaneClient.h"
#include "PlaneClient_Internal.h"

#include "IssueDraft.h"
#include "Logger.h"
#include "MarkdownConvert.h"
#include "StringUtil.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cpr/cpr.h>
#include <string>

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

TrackerError PlaneClient::UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) {
    std::string outError;
    // Resolve config + headers + project + UUID under the cache lock, then drop the lock before
    // the HTTP PATCH so UI thread calls to ResolveDisplayValue / display-name lookups are not
    // blocked for the duration of the round trip.
    TrackerConfig cfg = ConfigManager::Load();
    // PR 6: legacy cfg.PlaneProjectId removed. Resolve via the active view query (filled by the
    // current view; legacy installs swept by PR 5). UpdateIssueFields is invoked from grid edits
    // which are scoped to the current view → its query carries the project.
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
        if (planeProjectId_.empty()) {
            // First-call HTTP under the lock is acceptable: it runs at most once per process
            // lifetime; the steady-state PATCH path below is the hot one that needed the fix.
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, planeProjectId_, planeProjectIdentifier_,
                                &outError);
        }
        if (planeProjectId_.empty()) {
            // ResolvePlaneProject failure: its inner HTTP status is not surfaced here; wrap as Unknown
            // (Detail preserved verbatim). TODO(#21b later slice): re-thread status when a consumer reads .Kind.
            return TrackerErrorUnknown(outError);
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

    auto response = TrackerPatchLogged("PlaneClient", url, headers, fields.dump());
    LogTrackerHttpResult("PlaneClient", "PATCH", url, response);

    if (response.status_code != 200 && response.status_code != 204) {
        std::string detail;
        try {
            auto j = nlohmann::json::parse(response.text);
            if (j.is_object() && j.contains("error"))
                detail = j["error"].dump();
            else if (j.is_object() && j.contains("detail"))
                detail = j["detail"].dump();
            else
                detail = response.text;
        } catch (...) { // catch-all-ok: error body not JSON — fall back to the raw response text
            detail = response.text;
        }
        outError = "Plane API error: " + std::to_string(response.status_code) + " " + detail;
        // 2xx-but-not-200/204 (e.g. 206) reaches this failure branch; guard before FromHttpStatus,
        // which would map a 2xx to Ok() and silently drop the detail (plan FIX-1 / Slice-2 precedent).
        if (response.status_code >= 200 && response.status_code < 300) {
            return TrackerErrorUnknown(outError, response.status_code);
        }
        return TrackerErrorFromHttpStatus(response.status_code, outError);
    }
    return TrackerError::Ok();
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
        // RICH_TEXT_EDITING_V2_PLAN.md.
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
        auto uIt =
            std::find_if(cachedUsers_.begin(), cachedUsers_.end(), [&](const auto& u) { return u.AccountId == value; });
        if (uIt != cachedUsers_.end())
            return uIt->DisplayName;
        // Fallback to searching AllowedValueOptions if not in cachedUsers_
        auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                  [&](const auto& opt) { return opt.Id == value; });
        if (optIt != field->AllowedValueOptions.end())
            return optIt->Value;
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
        // PR 6: legacy cfg.PlaneProjectId removed (see remove-global-project-key.md §2.5).
        // Resolve from the active view's query — same pattern as UpdateIssueFields above.
        const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);
        planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
        for (const auto& kv : BuildPlaneHeaders(cfg)) {
            headers.insert({kv.first, kv.second});
        }
        workspaceSlug = cfg.PlaneWorkspaceSlug;
        if (planeProjectId_.empty()) {
            ResolvePlaneProject(planeApi, cfg, projectKey, headers, planeProjectId_, planeProjectIdentifier_,
                                &outError);
        }
        if (planeProjectId_.empty()) {
            // ResolvePlaneProject failure: inner HTTP status not surfaced here; wrap Unknown (Detail
            // verbatim). TODO(#21b later slice): re-thread status when a consumer reads .Kind.
            return Result<std::string, TrackerError>::Err(TrackerErrorUnknown(outError));
        }
        resolvedProjectId = planeProjectId_;
        projectIdentifier = planeProjectIdentifier_;
    }

    const std::string url =
        planeApi + "/api/v1/workspaces/" + workspaceSlug + "/projects/" + resolvedProjectId + "/work-items/";

    auto response = TrackerPostLogged("PlaneClient", url, headers, fields.dump());
    LogTrackerHttpResult("PlaneClient", "POST", url, response);

    if (response.status_code != 200 && response.status_code != 201) {
        std::string detail;
        try {
            auto j = nlohmann::json::parse(response.text);
            // Plane often returns {"error": "..."} or {"detail": "..."} or a dict of field errors
            if (j.is_object()) {
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
        outError = "Plane API error: " + std::to_string(response.status_code) + " " + detail;
        // 2xx-but-not-200/201 reaches this failure branch; guard before FromHttpStatus (FIX-1 / Slice-2).
        if (response.status_code >= 200 && response.status_code < 300) {
            return Result<std::string, TrackerError>::Err(TrackerErrorUnknown(outError, response.status_code));
        }
        return Result<std::string, TrackerError>::Err(TrackerErrorFromHttpStatus(response.status_code, outError));
    }

    try {
        auto j = nlohmann::json::parse(response.text);
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
                                                                     const std::vector<TrackerField>& /*catalog*/) {
    nlohmann::json outPayload = nlohmann::json::object();

    outPayload["name"] = draft.FieldValues.count("summary") ? draft.FieldValues.at("summary") : "";

    if (draft.FieldValues.count("description")) {
        const std::string& desc = draft.FieldValues.at("description");
        if (!desc.empty()) {
            // New-issue draft also goes through the modal-style Markdown surface; convert to the
            // HTML subset Plane accepts. See RICH_TEXT_EDITING_V2_PLAN.md.
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

    return Result<nlohmann::json, TrackerError>::Ok(std::move(outPayload));
}

Result<nlohmann::json, TrackerError> PlaneClient::BuildUpdatePayload(const IssueDraft& draft,
                                                                     const std::vector<TrackerField>& catalog) {
    // For Plane, update payload is the same as create payload (subset of fields)
    return BuildCreatePayload(draft, catalog);
}
