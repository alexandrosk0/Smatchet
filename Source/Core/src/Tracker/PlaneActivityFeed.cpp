#include "PlaneActivityFeed.h"

#include "PlaneClient.h"
#include "PlaneClient_Internal.h"
#include "Logger.h"
#include "StringUtil.h"
#include "PlaneIssueMappingPure.h"
#include "TrackerHttpUtils.h"
#include "Json/BoundedJsonParse.h"

#include <cpr/cpr.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

// The pure mapping half (EntriesFromActivitiesJson / WorkItemWorthScanning) lives in
// PlaneActivityFeedPure.cpp so the doctest rig links it without this TU's cpr +
// PlaneClient dependencies.

namespace {

cpr::Header ToCprHeader(const std::unordered_map<std::string, std::string>& headers) {
    cpr::Header out;
    for (const auto& kv : headers) {
        out[kv.first] = kv.second;
    }
    return out;
}

/// Display key for a discovered work item: "PROJ-123" when the project identifier
/// and sequence are known, "#123" with sequence only, the raw UUID otherwise.
std::string IssueKeyForWorkItem(const nlohmann::json& item, const std::string& projectIdentifier,
                                const std::string& issueId) {
    const std::string sequence = smatchet::plane_detail::JsonFieldToString(item, "sequence_id");
    if (!projectIdentifier.empty() && !sequence.empty()) {
        return projectIdentifier + "-" + sequence;
    }
    if (!sequence.empty()) {
        return "#" + sequence;
    }
    return issueId;
}

/// Paginated per-issue /activities/ scan appending the actor's in-window entries.
/// Degrades per-issue (LOG_WARN + stop this issue) so one bad issue doesn't void
/// the whole window.
void ScanIssueActivities(const std::string& listBase, const std::string& issueId, const cpr::Header& headers,
                         const std::string& accountId, const std::string& dayFrom, const std::string& dayTo,
                         const std::string& issueKey, const std::string& issueSummary, const std::string& issueUrl,
                         const std::atomic<bool>& cancel, std::vector<TrackerActivityEntry>& outEntries) {
    const int kMaxActivityPages = 5; // per-issue activity pagination bound
    std::string activityCursor;
    for (int activityPage = 1; activityPage <= kMaxActivityPages; ++activityPage) {
        if (cancel.load()) {
            return;
        }
        cpr::Parameters activityParams{{"per_page", "100"}};
        if (!activityCursor.empty()) {
            activityParams.Add({"cursor", activityCursor});
        }
        auto activityResp =
            TrackerGetLogged("PlaneClient", listBase + issueId + "/activities/", headers, activityParams);
        if (activityResp.status_code != 200) {
            LOG_WARN("PlaneClient: activities fetch failed: HTTP %ld issue=%s",
                     static_cast<long>(activityResp.status_code), TruncateForLog(issueKey, 40).c_str());
            return;
        }
        std::string parseErr;
        nlohmann::json activityPayload = smatchet::json_safe::ParseBounded(activityResp.text, parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("PlaneClient: activities parse error issue=%s: %s", TruncateForLog(issueKey, 40).c_str(),
                     parseErr.c_str());
            return;
        }
        // json::value(key, default) throws type_error on a non-object; a hostile
        // payload could be a top-level array/scalar, so guard before reading "results".
        if (!activityPayload.is_object()) {
            LOG_WARN("PlaneClient: activities payload not an object issue=%s", TruncateForLog(issueKey, 40).c_str());
            return;
        }
        std::vector<TrackerActivityEntry> entries =
            PlaneActivityFeed::EntriesFromActivitiesJson(activityPayload.value("results", nlohmann::json::array()),
                                                         accountId, dayFrom, dayTo, issueKey, issueSummary, issueUrl);
        for (auto& entry : entries) {
            outEntries.push_back(std::move(entry));
        }
        activityCursor = smatchet::plane::NextPaginationCursor(activityPayload);
        if (activityCursor.empty()) {
            return;
        }
    }
}

} // namespace

Result<std::vector<TrackerActivityEntry>, TrackerError>
PlaneClient::FetchUserActivity(const TrackerConfig& cfg, const std::string& accountId, const std::string& dayFrom,
                               const std::string& dayTo, const std::string& projectScope,
                               TrackerActivityProgress& progress) {
    using FeedResult = Result<std::vector<TrackerActivityEntry>, TrackerError>;
    using smatchet::plane_detail::JsonFieldToString;
    std::vector<TrackerActivityEntry> outEntries;
    if (cfg.PlaneUrl.empty() || cfg.PlaneApiKey.empty() || cfg.PlaneWorkspaceSlug.empty()) {
        return FeedResult::Err(TrackerErrorAuth("Missing Plane URL, API key, or workspace slug."));
    }
    if (accountId.empty()) {
        return FeedResult::Ok(std::move(outEntries));
    }
    activityCancel_.store(false);

    const std::string planeApi = smatchet::plane_detail::NormalizePlaneApiBase(cfg.PlaneUrl);
    const cpr::Header headers = ToCprHeader(BuildPlaneHeaders(cfg));

    // Plane's work-item + activity endpoints are project-scoped — resolve the scope
    // hint (falling back to the view query's project) before any scan.
    std::string projectKey = projectScope;
    if (projectKey.empty()) {
        projectKey = ExtractProjectFromQuery(cfg.JqlQuery);
    }
    if (projectKey.empty()) {
        return FeedResult::Err(
            TrackerErrorInvalidRequest("Plane activity requires a project scope (set a project in the view query)."));
    }
    std::string projectId;
    std::string projectIdentifier;
    std::string resolveError;
    TrackerError resolveClassified;
    if (!smatchet::plane_detail::ResolvePlaneProject(planeApi, cfg, projectKey, headers, projectId, projectIdentifier,
                                                     &resolveError, &resolveClassified)) {
        LOG_ERROR("PlaneClient: activity project resolve failed: %s", resolveError.c_str());
        // Classified at the resolve failure site (N12 slice 3).
        return FeedResult::Err(resolveClassified.IsOk() ? TrackerErrorUnknown(resolveError) : resolveClassified);
    }

    const std::string listBase =
        planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + projectId + "/work-items/";

    // Discovery: scan the project's work items (newest pages first is not guaranteed,
    // so the updated_at skip rule bounds the per-issue activity fetches instead).
    const int kMaxDiscoveryPages = 10; // 1000 issues — bounded scan, not a full sync
    std::string cursor;
    std::string outError;
    for (int page = 1; page <= kMaxDiscoveryPages; ++page) {
        if (activityCancel_.load()) {
            break; // window closed — return what we have
        }
        cpr::Parameters params{{"per_page", "100"}};
        if (!cursor.empty()) {
            params.Add({"cursor", cursor});
        }
        auto resp = TrackerGetLogged("PlaneClient", listBase, headers, params);
        if (resp.status_code != 200) {
            outError = "activity discovery failed: HTTP " + std::to_string(resp.status_code);
            LOG_ERROR("PlaneClient: %s account=%s", outError.c_str(), TruncateForLog(accountId, 40).c_str());
            if (resp.status_code >= 200 && resp.status_code < 300) {
                return FeedResult::Err(TrackerErrorUnknown(outError, resp.status_code));
            }
            return FeedResult::Err(TrackerErrorFromHttpStatus(resp.status_code, outError));
        }
        std::string parseErr;
        // SMATCHET_DEVIATION(rule=duplication; reason=ParseBounded + parseErr-check + is_object guard is the shared
        // bounded-ingress shape surfaced across independent tracker clients by the security sweep; de-duping into a
        // shared helper would couple unrelated Jira/Plane subsystems and is DRY-CRITICAL to avoid;
        // owner=security-audit; revisit=2026-09-30)
        nlohmann::json listPayload = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("activity discovery parse error: ") + parseErr;
            LOG_ERROR("PlaneClient: %s", outError.c_str());
            return FeedResult::Err(TrackerErrorParse(outError));
        }
        // json::value(key, default) throws type_error on a non-object; guard the
        // discovery payload the same way before reading "results".
        if (!listPayload.is_object()) {
            LOG_WARN("PlaneClient: activity discovery payload not an object account=%s",
                     TruncateForLog(accountId, 40).c_str());
            break;
        }
        const auto results = listPayload.value("results", nlohmann::json::array());
        if (results.is_array()) {
            progress.Total.fetch_add(static_cast<int>(results.size()));
            for (const auto& item : results) {
                if (activityCancel_.load()) {
                    break;
                }
                if (!item.is_object()) {
                    progress.Current.fetch_add(1);
                    continue;
                }
                const std::string issueId = JsonFieldToString(item, "id");
                const std::string updatedAt = JsonFieldToString(item, "updated_at");
                if (issueId.empty() || !PlaneActivityFeed::WorkItemWorthScanning(updatedAt, dayFrom)) {
                    progress.Current.fetch_add(1);
                    continue;
                }
                const std::string issueKey = IssueKeyForWorkItem(item, projectIdentifier, issueId);
                const std::string issueSummary = JsonFieldToString(item, "name");
                const std::string issueUrl = BuildBrowseUrl(cfg, issueKey);
                ScanIssueActivities(listBase, issueId, headers, accountId, dayFrom, dayTo, issueKey, issueSummary,
                                    issueUrl, activityCancel_, outEntries);
                progress.Current.fetch_add(1);
            }
        }
        cursor = smatchet::plane::NextPaginationCursor(listPayload);
        if (cursor.empty()) {
            break;
        }
    }

    std::stable_sort(outEntries.begin(), outEntries.end(),
                     [](const TrackerActivityEntry& a, const TrackerActivityEntry& b) {
                         return a.Timestamp > b.Timestamp; // newest first
                     });
    return FeedResult::Ok(std::move(outEntries));
}

Result<std::vector<std::string>, TrackerError> PlaneClient::FetchUserGroupNames(const TrackerConfig& /*cfg*/,
                                                                                const std::string& /*accountId*/) {
    // Plane has no group concept — Ok-empty (supported, nothing to show), not the
    // default-deny Err (plan user-info-window.md item 17).
    return Result<std::vector<std::string>, TrackerError>::Ok(std::vector<std::string>());
}

Result<std::vector<TrackerUser>, TrackerError> PlaneClient::FetchGroupMembers(const TrackerConfig& /*cfg*/,
                                                                              const std::string& /*groupName*/) {
    return Result<std::vector<TrackerUser>, TrackerError>::Ok(std::vector<TrackerUser>());
}

void PlaneClient::ClearUserActivity() { activityCancel_.store(true); }
