#include "PlaneClient.h"
#include "PlaneClient_Internal.h"

#include "Logger.h"
#include "StringUtil.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cpr/cpr.h>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

using smatchet::plane_detail::JsonFieldToString;
using smatchet::plane_detail::NormalizePlaneApiBase;
using smatchet::plane_detail::ResolvePlaneProject;
using smatchet::plane_detail::TrimAsciiWs;

namespace {

bool LooksHtmlPrefix(const std::string& t) {
    size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])) != 0) {
        ++i;
    }
    return i < t.size() && t[i] == '<';
}

std::string SanitizeAsciiSnippet(const std::string& s, size_t maxLen) {
    std::string o;
    o.reserve((std::min)(maxLen, s.size()));
    for (size_t i = 0; i < s.size() && o.size() < maxLen; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            o += static_cast<char>(c);
        } else {
            o += '?';
        }
    }
    return o;
}

std::string PlaneWorkItemTitleForDisplay(const nlohmann::json& issue) {
    std::string title = JsonFieldToString(issue, "name");
    TrimAsciiWs(title);
    return title;
}

// Pull `project_id` from a Plane structured-query JSON blob. "" sentinel covers
// both "no project_id key" and "malformed JSON" — callers must fall back.
std::string ExtractProjectFromPlaneQuery(const std::string& planeQueryJson) {
    if (planeQueryJson.empty()) {
        return "";
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(planeQueryJson);
        if (j.is_object()) {
            auto it = j.find("project_id");
            if (it != j.end() && it->is_string()) {
                return it->get<std::string>();
            }
            auto it2 = j.find("project");
            if (it2 != j.end() && it2->is_string()) {
                return it2->get<std::string>();
            }
        }
    } catch (const std::exception&) {
        return "";
    } catch (...) {
        return "";
    }
    return "";
}

constexpr std::int64_t kPlaneListProjectsTtlSeconds = 300; // 5 minutes

std::int64_t PlaneNowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::vector<CachedTicket> PlaneClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                                   const ViewsStore* viewsOverride, std::string* outFetchError,
                                                   std::string* outWarning) {
    std::vector<CachedTicket> results;
    auto onBatch = [&](std::vector<CachedTicket>&& batch) {
        results.insert(results.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    };
    auto shouldCancel = []() { return false; };
    TrackerIssueFetchSummary summary = FetchIssuesStreamed(onBatch, shouldCancel, configOverride, viewsOverride);
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = summary.FullSyncCompleted;
    }
    if (outFetchError) {
        *outFetchError = summary.FetchError;
    }
    if (outWarning) {
        *outWarning = summary.Warning;
    }
    return results;
}

TrackerIssueFetchSummary PlaneClient::FetchIssuesStreamed(const BatchCallback& onBatch,
                                                          const CancelCallback& shouldCancel,
                                                          const TrackerConfig* configOverride,
                                                          const ViewsStore* /*viewsOverride*/) {

    TrackerIssueFetchSummary summary;

    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();

    // PR 6: legacy global cfg.PlaneProjectId removed. The active project is extracted from the
    // active view's query (PR 5 sweep ensures legacy views carry their project in the saved
    // query). Empty here ≡ "no project scope" — surfaces the same "configure" error as before.
    // See docs/design/applied/remove-global-project-key.md §2.5 / §7 PR 6.
    const std::string projectKey = ExtractProjectFromQuery(cfg.JqlQuery);

    if (cfg.PlaneUrl.empty() || cfg.PlaneWorkspaceSlug.empty() || projectKey.empty()) {
        summary.FetchError = "Plane is not configured or active view has no project scope. "
                             "Set URL / Workspace Slug in Preferences, and choose a Plane view with a project.";
        return summary;
    }
    if (cfg.PlaneApiKey.empty()) {
        summary.FetchError = "Plane API key is missing. Set it in Preferences → Tracker.";
        return summary;
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    if (planeApi != cfg.PlaneUrl) {
        LOG_INFO(
            "PlaneClient::FetchIssuesStreamed: REST base %s (configured URL was web app host; use https://api.plane.so "
            "in preferences to skip this rewrite).",
            planeApi.c_str());
    }

    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    std::string prevProjectId;
    std::string prevProjectIdentifier;
    std::vector<TrackerUser> localCachedUsers;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        prevProjectId = planeProjectId_;
        prevProjectIdentifier = planeProjectIdentifier_;
        localCachedUsers = cachedUsers_;
    }

    std::string resolveError;
    std::string tempProjectId = prevProjectId;
    std::string tempProjectIdentifier = prevProjectIdentifier;
    if (!ResolvePlaneProject(planeApi, cfg, projectKey, headers, tempProjectId, tempProjectIdentifier, &resolveError)) {
        summary.FetchError = resolveError;
        return summary;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        planeProjectId_ = tempProjectId;
        planeProjectIdentifier_ = tempProjectIdentifier;
        if (planeProjectId_ != prevProjectId) {
            keyToId_.clear();
        }
    }
    const std::string planeProjectId = tempProjectId;

    std::unordered_map<std::string, std::string> localKeyToId;

    try {
        // Fetch states for display value mapping
        {
            const std::string statesUrl =
                planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" + planeProjectId + "/states/";
            auto r = TrackerGetLogged("PlaneClient", statesUrl, headers);
            if (r.status_code == 200) {
                const std::string statesBody = StripUtf8BomCopy(r.text);
                nlohmann::json j = nlohmann::json::parse(statesBody, nullptr, false);
                if (!j.is_discarded()) {
                    std::vector<CachedState> tempCachedStates;
                    auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
                    if (results.is_array()) {
                        for (const auto& s : results) {
                            CachedState cs;
                            cs.Id = JsonFieldToString(s, "id");
                            cs.Name = JsonFieldToString(s, "name");
                            if (!cs.Id.empty())
                                tempCachedStates.push_back(cs);
                        }
                    }
                    {
                        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
                        cachedStates_ = std::move(tempCachedStates);
                    }
                }
            }
        }

        const int pageSize = 100;
        // Hard cap on outer pagination to bound a misbehaving cursor that loops back to itself.
        // 50 pages × 100 work-items/page = 5,000 issues, comfortably above any active-view JQL
        // and matches the per-server safety limit in JiraIssueSearch.cpp.
        constexpr int kMaxPlanePages = 50;
        std::string listCursor;
        bool syncEndedCleanly = false;
        int pageCount = 0;

        while (true) {
            if (shouldCancel && shouldCancel()) {
                syncEndedCleanly = false;
                break;
            }

            if (pageCount >= kMaxPlanePages) {
                const std::string warn = "Plane pagination outer page cap (" + std::to_string(kMaxPlanePages) +
                                         ") reached; remaining issues not fetched. Narrow your view or raise the cap.";
                LOG_WARN("PlaneClient::FetchIssuesStreamed %s", warn.c_str());
                // Soft warning: the issues we did fetch are valid; treat as a partial success
                // rather than a fetch failure so the UI fires its success notify and the
                // connectivity banner stays clear.
                summary.Warning = warn;
                break;
            }
            ++pageCount;

            const std::string listBase = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/" +
                                         planeProjectId + "/work-items/";
            cpr::Parameters params;
            params.Add({"per_page", std::to_string(pageSize)});
            params.Add({"expand", "assignees,labels,state"});
            if (!listCursor.empty()) {
                params.Add({"cursor", listCursor});
            }

            auto response = TrackerGetLogged("PlaneClient", listBase, headers, params);

            if (response.status_code != 200) {
                const std::string urlHint = SanitizeAsciiSnippet(listBase, 200);
                std::string apiDetail;
                {
                    const std::string tb = StripUtf8BomCopy(response.text);
                    const nlohmann::json ej = nlohmann::json::parse(tb, nullptr, false);
                    if (!ej.is_discarded() && ej.is_object() && ej.contains("detail")) {
                        apiDetail = JsonFieldToString(ej, "detail");
                    }
                }
                std::string err = "Plane API error " + std::to_string(response.status_code) +
                                  " fetching issues (URL: " + urlHint + "): " + response.text.substr(0, 300);
                if (!apiDetail.empty()) {
                    err += " [detail: " + apiDetail + "]";
                }
                if (response.status_code == 404) {
                    err += " Check Workspace Slug and Project ID (UUID from app URL or Plane settings), and API key "
                           "scopes.";
                }
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            const std::string bodyForJson = StripUtf8BomCopy(response.text);
            const bool looksHtml = LooksHtmlPrefix(bodyForJson);

            if (bodyForJson.empty()) {
                const std::string err = "Plane returned empty response body (HTTP 200) when fetching issues.";
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }
            if (looksHtml) {
                const std::string urlHint = SanitizeAsciiSnippet(listBase, 220);
                const std::string err =
                    "Plane returned HTML instead of JSON (HTTP 200). Request URL: " + urlHint +
                    ". For Plane Cloud set base URL to https://api.plane.so (origin only, no /workspace path). "
                    "Self-hosted: use the API origin your reverse proxy serves for /api/v1/.";
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            nlohmann::json j = nlohmann::json::parse(bodyForJson, nullptr, false);
            if (j.is_discarded()) {
                const std::string err =
                    "Plane returned invalid JSON when fetching issues (HTTP 200). Verify Plane URL, workspace slug, "
                    "project UUID, and API key.";
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
            if (!results.is_array()) {
                std::string keyList;
                if (j.is_object()) {
                    for (auto it = j.begin(); it != j.end() && keyList.size() < 240; ++it) {
                        if (!keyList.empty())
                            keyList += ',';
                        keyList += it.key();
                    }
                }
                const std::string err =
                    "Plane list response has no results array (wrong endpoint or API version). Top-level keys: " +
                    keyList;
                LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", err.c_str());
                summary.FetchError = err;
                return summary;
            }

            if (results.empty()) {
                syncEndedCleanly = true;
                break;
            }

            std::vector<CachedTicket> pageIssues;
            for (const auto& issue : results) {
                if (shouldCancel && shouldCancel()) {
                    break;
                }
                try {
                    CachedTicket ticket;
                    const std::string uuid = JsonFieldToString(issue, "id");
                    const std::string seqId = JsonFieldToString(issue, "sequence_id");

                    std::string visualKey;
                    if (!tempProjectIdentifier.empty() && !seqId.empty()) {
                        visualKey = tempProjectIdentifier + "-" + seqId;
                    } else if (!seqId.empty()) {
                        visualKey = "#" + seqId;
                    } else {
                        visualKey = uuid;
                    }

                    ticket.id = visualKey;
                    ticket.fieldValues["uuid"] = uuid;
                    ticket.fieldValues["key"] = visualKey;
                    localKeyToId[visualKey] = uuid;
                    ticket.fieldValues["summary"] = PlaneWorkItemTitleForDisplay(issue);

                    // Status
                    if (issue.contains("state_detail") && issue["state_detail"].is_object()) {
                        ticket.fieldValues["status"] = JsonFieldToString(issue["state_detail"], "id");
                    } else if (issue.contains("state") && issue["state"].is_object()) {
                        ticket.fieldValues["status"] = JsonFieldToString(issue["state"], "id");
                    } else {
                        ticket.fieldValues["status"] = JsonFieldToString(issue, "state");
                    }

                    // Assignee
                    std::string assigneeId;
                    if (issue.contains("assignees") && issue["assignees"].is_array() && !issue["assignees"].empty()) {
                        const auto& first = issue["assignees"][0];
                        if (first.is_object()) {
                            assigneeId = JsonFieldToString(first, "id");
                        } else if (first.is_string()) {
                            assigneeId = first.get<std::string>();
                        }
                    }

                    std::string assigneeName = assigneeId;
                    if (issue.contains("assignee_details") && issue["assignee_details"].is_array() &&
                        !issue["assignee_details"].empty()) {
                        const auto& first = issue["assignee_details"][0];
                        if (first.is_object() && first.contains("display_name")) {
                            assigneeName = JsonFieldToString(first, "display_name");
                        }
                    }

                    if (assigneeName == assigneeId && !assigneeId.empty()) {
                        auto uIt = std::find_if(localCachedUsers.begin(), localCachedUsers.end(),
                                                [&](const auto& u) { return u.AccountId == assigneeId; });
                        if (uIt != localCachedUsers.end()) {
                            assigneeName = uIt->DisplayName;
                        }
                    }

                    ticket.fieldValues["assignee"] = assigneeName;
                    ticket.fieldValues["priority"] = JsonFieldToString(issue, "priority");

                    // Sprint
                    if (issue.contains("cycle_details") && issue["cycle_details"].is_object()) {
                        ticket.fieldValues["sprint"] = JsonFieldToString(issue["cycle_details"], "id");
                    } else if (issue.contains("cycle") && issue["cycle"].is_object()) {
                        ticket.fieldValues["sprint"] = JsonFieldToString(issue["cycle"], "id");
                    } else {
                        ticket.fieldValues["sprint"] = JsonFieldToString(issue, "cycle");
                    }

                    // Labels
                    std::string labelStr;
                    if (issue.contains("label_details") && issue["label_details"].is_array()) {
                        for (const auto& lbl : issue["label_details"]) {
                            if (!labelStr.empty())
                                labelStr += ", ";
                            std::string ln = JsonFieldToString(lbl, "name");
                            if (ln.empty())
                                ln = JsonFieldToString(lbl, "id");
                            labelStr += ln;
                        }
                    } else if (issue.contains("labels") && issue["labels"].is_array()) {
                        for (const auto& lbl : issue["labels"]) {
                            if (!labelStr.empty())
                                labelStr += ", ";
                            if (lbl.is_object()) {
                                std::string ln = JsonFieldToString(lbl, "name");
                                if (ln.empty())
                                    ln = JsonFieldToString(lbl, "id");
                                labelStr += ln;
                            } else if (lbl.is_string()) {
                                labelStr += lbl.get<std::string>();
                            }
                        }
                    }
                    ticket.fieldValues["labels"] = labelStr;

                    ticket.fieldValues["created"] = JsonFieldToString(issue, "created_at");
                    ticket.fieldValues["updated"] = JsonFieldToString(issue, "updated_at");
                    ticket.fieldValues["description"] = JsonFieldToString(issue, "description_stripped");
                    // Preserve the original HTML so the long-text modal editor can round-trip via
                    // Markdown without destroying formatting on save. See RICH_TEXT_EDITING_V2_PLAN.md.
                    const std::string descHtml = JsonFieldToString(issue, "description_html");
                    if (!descHtml.empty()) {
                        ticket.fieldRichValues["description"] = descHtml;
                    }

                    // Issue Type
                    if (issue.contains("type_detail") && issue["type_detail"].is_object()) {
                        ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type_detail"], "name");
                    } else if (issue.contains("type") && issue["type"].is_object()) {
                        ticket.fieldValues["issuetype"] = JsonFieldToString(issue["type"], "name");
                    } else {
                        ticket.fieldValues["issuetype"] = JsonFieldToString(issue, "type");
                    }

                    if (!ticket.id.empty()) {
                        pageIssues.push_back(std::move(ticket));
                    }
                } catch (const std::exception& ex) {
                    LOG_WARN("PlaneClient::FetchIssuesStreamed: skipping issue parse error: %s", ex.what());
                }
            }

            if (shouldCancel && shouldCancel()) {
                syncEndedCleanly = false;
                break;
            }

            size_t added = pageIssues.size();
            summary.FetchedCount += added;

            if (onBatch && added > 0) {
                onBatch(std::move(pageIssues));
            }

            // Cursor pagination
            listCursor.clear();
            bool more = false;
            if (j.is_object()) {
                if (j.contains("next_page_results") && j["next_page_results"].is_boolean()) {
                    more = j["next_page_results"].get<bool>();
                }
                if (more && j.contains("next_cursor") && j["next_cursor"].is_string()) {
                    listCursor = j["next_cursor"].get<std::string>();
                }
            }
            if (!more || listCursor.empty()) {
                syncEndedCleanly = true;
                break;
            }
        }
        summary.FullSyncCompleted = syncEndedCleanly && (!shouldCancel || !shouldCancel());
        LOG_INFO("PlaneClient::FetchIssuesStreamed fetched %zu issues from Plane.", summary.FetchedCount);
    } catch (const nlohmann::json::exception& jex) {
        LOG_ERROR("PlaneClient::FetchIssuesStreamed outer json error: %s", jex.what());
        summary.FetchError = std::string("Plane sync failed (JSON): ") + jex.what();
    } catch (const std::exception& ex) {
        LOG_ERROR("PlaneClient::FetchIssuesStreamed outer error: %s", ex.what());
        summary.FetchError = std::string("Plane sync failed: ") + ex.what();
    } catch (...) {
        LOG_ERROR("PlaneClient::FetchIssuesStreamed outer error: unknown exception");
        summary.FetchError = "Plane sync failed: unknown exception";
    }

    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        for (const auto& kv : localKeyToId) {
            keyToId_[kv.first] = kv.second;
        }
    }

    return summary;
}

TrackerReachabilityProbeResult PlaneClient::ProbeReachability(const TrackerConfig& cfg) {
    TrackerReachabilityProbeResult out;
    if (cfg.PlaneUrl.empty() || cfg.PlaneApiKey.empty()) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "Missing Plane URL or API key.";
        return out;
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    const std::string url = planeApi + "/api/v1/users/me/";
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers.insert({kv.first, kv.second});
    }

    // Route through TrackerGetLogged so NetworkUsageTracker + body-trace logging stay in the
    // loop (was a §2.1 P1 bug: raw cpr::Get bypassed both). Classification via TrackerError
    // also fixes the second §2.1 P1: a 404 from a stale base URL was wrongly TransportDown.
    const cpr::Response resp =
        TrackerGetLogged("PlaneClient", url, headers, kTrackerProbeConnectTimeoutMs, kTrackerProbeOverallTimeoutMs);
    const TrackerHttpResult classified = ClassifyTrackerResponse(resp);

    switch (classified.Error.Kind) {
    case TrackerErrorKind::None:
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = "HTTP 200";
        break;
    case TrackerErrorKind::Auth:
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status()) + " (Auth Error)";
        break;
    case TrackerErrorKind::ServerError:
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status()) + " (Server Error)";
        break;
    case TrackerErrorKind::NotFound:
    case TrackerErrorKind::InvalidRequest:
    case TrackerErrorKind::RateLimited:
        // Reachable, but the response indicates a config / payload issue rather than transport
        // failure. Surfaces as the auth-or-config banner so a stale base URL or rate-limited
        // probe doesn't flip the connectivity banner to "offline".
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = "HTTP " + std::to_string(classified.Status());
        break;
    case TrackerErrorKind::Transport:
    default:
        out.Kind = TrackerReachabilityProbeKind::TransportDown;
        out.Diagnostic = resp.error.message.empty() ? classified.Error.Detail : resp.error.message;
        break;
    }
    return out;
}

bool PlaneClient::FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                     const ViewsStore& /*views*/, std::vector<CachedTicket>& outTickets,
                                     std::string& outError) {
    if (issueKeys.empty()) {
        return true;
    }

    // Stream pages and early-exit once every requested key has been found. Cuts wall-time and
    // HTTP cost on hot prefetch-open-links paths where the targeted keys typically live on the
    // first one or two pages. Falls back to a full sweep only when keys are missing or spread
    // across the page cap. Server-side `sequence_id__in` filtering would be the next win — out
    // of scope here because it requires touching FetchIssuesStreamed's URL builder.
    std::unordered_set<std::string> wanted(issueKeys.begin(), issueKeys.end());
    std::unordered_set<std::string> found;
    found.reserve(wanted.size());

    auto onBatch = [&](std::vector<CachedTicket>&& batch) {
        for (auto& t : batch) {
            const std::string keyField = t.GetFieldValue("key");
            const bool match = wanted.count(t.id) > 0 || (!keyField.empty() && wanted.count(keyField) > 0);
            if (!match) {
                continue;
            }
            if (!t.id.empty()) {
                found.insert(t.id);
            }
            if (!keyField.empty()) {
                found.insert(keyField);
            }
            outTickets.push_back(std::move(t));
        }
    };
    auto shouldCancel = [&]() { return found.size() >= wanted.size(); };

    TrackerIssueFetchSummary summary = FetchIssuesStreamed(onBatch, shouldCancel, &cfg, nullptr);
    if (!summary.FetchError.empty()) {
        outError = summary.FetchError;
        return false;
    }
    return true;
}

std::string PlaneClient::ExtractProjectFromQuery(const std::string& query) const {
    return ExtractProjectFromPlaneQuery(query);
}

void PlaneClient::InvalidateListProjectsCache() {
    std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
    cachedProjects_.clear();
    cachedProjectsAtUnix_ = 0;
}

std::vector<RemoteProject> PlaneClient::ListProjects() {
    // Fast path: serve from cache when still warm.
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        const std::int64_t now = PlaneNowUnixSeconds();
        if (!cachedProjects_.empty() && (now - cachedProjectsAtUnix_) < kPlaneListProjectsTtlSeconds) {
            return cachedProjects_;
        }
    }

    const TrackerConfig cfg = ConfigManager::Load();
    if (cfg.PlaneWorkspaceSlug.empty()) {
        LOG_WARN("PlaneClient::ListProjects: PlaneWorkspaceSlug is empty.");
        return {};
    }

    const std::string planeApi = NormalizePlaneApiBase(cfg.PlaneUrl);
    cpr::Header headers;
    for (const auto& kv : BuildPlaneHeaders(cfg)) {
        headers[kv.first] = kv.second;
    }

    const std::string url = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/";
    cpr::Parameters params;
    params.Add({"per_page", "100"});

    const cpr::Response resp = TrackerGetLogged("PlaneClient", url, headers, params);
    if (resp.status_code != 200) {
        LOG_WARN("PlaneClient::ListProjects: HTTP %ld on %s", resp.status_code, url.c_str());
        return {};
    }

    std::vector<RemoteProject> projects;
    try {
        const nlohmann::json j = nlohmann::json::parse(StripUtf8BomCopy(resp.text), nullptr, false);
        if (j.is_discarded()) {
            LOG_WARN("PlaneClient::ListProjects: invalid JSON in response.");
            return {};
        }
        const auto& arr = (j.is_object() && j.contains("results")) ? j["results"] : j;
        if (!arr.is_array()) {
            LOG_WARN("PlaneClient::ListProjects: response has no results array.");
            return {};
        }
        projects.reserve(arr.size());
        for (const auto& p : arr) {
            if (!p.is_object()) {
                continue;
            }
            RemoteProject rp;
            rp.id = JsonFieldToString(p, "id");
            rp.key = JsonFieldToString(p, "identifier"); // may be empty
            rp.displayName = JsonFieldToString(p, "name");
            if (rp.id.empty()) {
                continue;
            }
            projects.push_back(std::move(rp));
        }
    } catch (const std::exception& ex) {
        LOG_WARN("PlaneClient::ListProjects: parse error: %s", ex.what());
        return {};
    } catch (...) {
        LOG_WARN("PlaneClient::ListProjects: parse error (unknown)");
        return {};
    }

    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        cachedProjects_ = projects;
        cachedProjectsAtUnix_ = PlaneNowUnixSeconds();
    }
    return projects;
}
