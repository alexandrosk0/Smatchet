#include "PlaneClient.h"
#include "PlaneClient_Internal.h"
#include "PlaneIssueMappingPure.h"

#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "ProjectResolver.h"
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
#include <utility>

using smatchet::plane_detail::JsonFieldToString;
using smatchet::plane_detail::NormalizePlaneApiBase;
using smatchet::plane_detail::ResolvePlaneProject;

namespace {

// Widen neutral id-and-name pairs into a typed cache vector. Templated so it
// works for any cache struct that carries an Id and a Name member.
template <typename CachedT>
std::vector<CachedT> PairsToCached(std::vector<std::pair<std::string, std::string>>& pairs) {
    std::vector<CachedT> out;
    out.reserve(pairs.size());
    for (auto& p : pairs) {
        CachedT c;
        c.Id = std::move(p.first);
        c.Name = std::move(p.second);
        out.push_back(std::move(c));
    }
    return out;
}

// Normalize the configured Plane URL to the REST API base, logging once when
// the configured value was the web-app host and got rewritten.
std::string NormalizeAndLogPlaneApiBase(const std::string& configuredUrl) {
    const std::string planeApi = NormalizePlaneApiBase(configuredUrl);
    if (planeApi != configuredUrl) {
        LOG_INFO(
            "PlaneClient::FetchIssuesStreamed: REST base %s (configured URL was web app host; use https://api.plane.so "
            "in preferences to skip this rewrite).",
            planeApi.c_str());
    }
    return planeApi;
}

// Convert a header name→value map into a cpr::Header.
cpr::Header ToCprHeader(const std::unordered_map<std::string, std::string>& kvs) {
    cpr::Header headers;
    for (const auto& kv : kvs) {
        headers.insert({kv.first, kv.second});
    }
    return headers;
}

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
    } catch (...) { // catch-all-ok: project-name lookup is best-effort; empty string on any parse failure
        return "";
    }
    return "";
}

constexpr std::int64_t kPlaneListProjectsTtlSeconds = 300; // 5 minutes

std::int64_t PlaneNowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ActiveViewJqlFromStore(const ViewsStore* viewsOverride) {
    if (viewsOverride == nullptr) {
        return std::string();
    }
    for (const ViewDefinition& view : viewsOverride->Views) {
        if (view.Id == viewsOverride->ActiveViewId) {
            return view.Jql;
        }
    }
    return std::string();
}

PlaneClient::BatchCallback WrapPlaneKeyFilterBatch(const std::string& keyFilter,
                                                   const PlaneClient::BatchCallback& onBatch) {
    if (keyFilter.empty() || !onBatch) {
        return onBatch;
    }
    return PlaneClient::BatchCallback([onBatch, keyFilter](std::vector<CachedTicket>&& batch) {
        std::vector<CachedTicket> kept;
        for (auto& t : batch) {
            const std::string keyField = t.GetFieldValue("key");
            if (t.id == keyFilter || (!keyField.empty() && keyField == keyFilter)) {
                kept.push_back(std::move(t));
            }
        }
        if (!kept.empty()) {
            onBatch(std::move(kept));
        }
    });
}

// Phase 1: validate Plane connection config + active-view project scope. Returns the
// fetch-error string on failure (empty on success). Byte-for-byte mirror of the original
// inline guards in FetchIssuesStreamed.
std::string ValidatePlaneFetchConfig(const TrackerConfig& cfg, const std::string& projectKey) {
    if (cfg.PlaneUrl.empty() || cfg.PlaneWorkspaceSlug.empty() || projectKey.empty()) {
        return "Plane is not configured or active view has no project scope. "
               "Set URL / Workspace Slug in Preferences, and choose a Plane view with a project.";
    }
    if (cfg.PlaneApiKey.empty()) {
        return "Plane API key is missing. Set it in Preferences → Tracker.";
    }
    return std::string();
}

// Phase 3: fetch + parse the project `states` list into (id, name) pairs (display-value mapping).
// Mirrors the original inline block exactly: only a 200 with a non-discarded body commits, and an
// empty-but-valid parse still commits (clears stale states). The outShouldCommit flag tells the
// caller when a parse is committable (even when empty) versus when the response was skipped. Returns
// neutral id-plus-name pairs rather than the private nested CachedState struct, so this free helper
// stays out of the class; the orchestrator widens the pairs into the typed cache.
std::vector<std::pair<std::string, std::string>>
FetchPlaneStatePairs(const std::string& planeApi, const std::string& workspaceSlug, const std::string& planeProjectId,
                     const cpr::Header& headers, bool& outShouldCommit) {
    outShouldCommit = false;
    std::vector<std::pair<std::string, std::string>> pairs;
    const std::string statesUrl =
        planeApi + "/api/v1/workspaces/" + workspaceSlug + "/projects/" + planeProjectId + "/states/";
    auto r = TrackerGetLogged("PlaneClient", statesUrl, headers);
    if (r.status_code != 200) {
        return pairs;
    }
    const std::string statesBody = StripUtf8BomCopy(r.text);
    std::string parseErr;
    nlohmann::json j = smatchet::json_safe::ParseBounded(statesBody, parseErr);
    if (!parseErr.empty()) {
        return pairs;
    }
    outShouldCommit = true;
    auto results = (j.is_object() && j.contains("results")) ? j["results"] : j;
    if (!results.is_array()) {
        return pairs;
    }
    for (const auto& s : results) {
        std::string id = JsonFieldToString(s, "id");
        std::string name = JsonFieldToString(s, "name");
        if (!id.empty())
            pairs.emplace_back(std::move(id), std::move(name));
    }
    return pairs;
}

// Outcome of fetching + validating a single work-items page. On a hard failure `Error` carries the
// caller-facing message (return immediately). On success `Results` aliases into `Body` (kept alive
// here) and `Body` drives cursor extraction.
struct PlaneIssuePageFetch {
    bool Ok = false;
    std::string Error;
    nlohmann::json Body;
};

// Phase 4: HTTP GET one work-items page + classify the response body. Reproduces the original
// inline non-200 / empty / HTML / invalid-JSON / no-results-array error paths byte-for-byte.
PlaneIssuePageFetch FetchPlaneIssuePage(const std::string& planeApi, const std::string& workspaceSlug,
                                        const std::string& planeProjectId, const cpr::Header& headers, int pageSize,
                                        const std::string& listCursor,
                                        const PlaneClient::CancelCallback& shouldCancel) {
    PlaneIssuePageFetch out;
    const std::string listBase =
        planeApi + "/api/v1/workspaces/" + workspaceSlug + "/projects/" + planeProjectId + "/work-items/";
    cpr::Parameters params;
    params.Add({"per_page", std::to_string(pageSize)});
    params.Add({"expand", "assignees,labels,state"});
    if (!listCursor.empty()) {
        params.Add({"cursor", listCursor});
    }

    // Thread the sync worker's cancellation token into the retry loop so an abort during a
    // backoff/retry window is honoured immediately, not only at the next page boundary.
    auto response = TrackerGetLogged("PlaneClient", listBase, headers, params, shouldCancel);

    if (response.status_code != 200) {
        const std::string urlHint = SanitizeAsciiSnippet(listBase, 200);
        std::string apiDetail;
        {
            const std::string tb = StripUtf8BomCopy(response.text);
            std::string detailParseErr;
            const nlohmann::json ej = smatchet::json_safe::ParseBounded(tb, detailParseErr);
            if (detailParseErr.empty() && ej.is_object() && ej.contains("detail")) {
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
        out.Error = err;
        return out;
    }

    const std::string bodyForJson = StripUtf8BomCopy(response.text);
    const bool looksHtml = LooksHtmlPrefix(bodyForJson);

    if (bodyForJson.empty()) {
        out.Error = "Plane returned empty response body (HTTP 200) when fetching issues.";
        return out;
    }
    if (looksHtml) {
        const std::string urlHint = SanitizeAsciiSnippet(listBase, 220);
        out.Error = "Plane returned HTML instead of JSON (HTTP 200). Request URL: " + urlHint +
                    ". For Plane Cloud set base URL to https://api.plane.so (origin only, no /workspace path). "
                    "Self-hosted: use the API origin your reverse proxy serves for /api/v1/.";
        return out;
    }

    std::string parseErr;
    nlohmann::json j = smatchet::json_safe::ParseBounded(bodyForJson, parseErr);
    if (!parseErr.empty()) {
        out.Error = "Plane returned invalid JSON when fetching issues (HTTP 200). Verify Plane URL, workspace slug, "
                    "project UUID, and API key.";
        return out;
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
        out.Error =
            "Plane list response has no results array (wrong endpoint or API version). Top-level keys: " + keyList;
        return out;
    }

    out.Ok = true;
    out.Body = std::move(j);
    return out;
}

// Translate cached TrackerUsers into the pure mapper's opaque (AccountId, DisplayName) lookup
// pairs once, ahead of the page loop. Byte-for-byte mirror of the original inline build.
std::vector<smatchet::plane::UserDisplayLookup> BuildPlaneUserLookups(const std::vector<TrackerUser>& cachedUsers) {
    std::vector<smatchet::plane::UserDisplayLookup> userLookup;
    userLookup.reserve(cachedUsers.size());
    for (const auto& u : cachedUsers) {
        smatchet::plane::UserDisplayLookup lk;
        lk.AccountId = u.AccountId;
        lk.DisplayName = u.DisplayName;
        userLookup.push_back(std::move(lk));
    }
    return userLookup;
}

// Outcome of the work-items pagination loop. `HardFailed` ≡ the original inline `return summary`
// (FetchError already on `summary`); the caller returns immediately. Otherwise the loop ran to a
// clean or warned termination and `EndedCleanly` / `PageCount` drive FullSyncCompleted.
struct PlanePageLoopResult {
    bool HardFailed = false;
    bool EndedCleanly = false;
    int PageCount = 0;
};

// Phase 4-6: drive the cursor-paginated work-items loop — fetch + classify each page, map its
// rows via the pure mapper, stream completed batches, and advance/terminate on the cursor.
// Reproduces the original inline loop byte-for-byte: cancel checks, outer page cap soft-warning,
// empty-results clean stop, and the per-page hard-failure early return.
PlanePageLoopResult
RunPlanePageLoop(const std::string& planeApi, const std::string& workspaceSlug, const std::string& planeProjectId,
                 const std::string& projectIdentifier, const cpr::Header& headers,
                 const std::vector<smatchet::plane::UserDisplayLookup>& userLookup,
                 const PlaneClient::BatchCallback& onBatch, const PlaneClient::CancelCallback& shouldCancel,
                 std::unordered_map<std::string, std::string>& localKeyToId, TrackerIssueFetchSummary& summary) {
    PlanePageLoopResult result;
    const int pageSize = 100;
    // Hard cap on outer pagination to bound a misbehaving cursor that loops back to itself.
    // 50 pages × 100 work-items/page = 5,000 issues, comfortably above any active-view JQL
    // and matches the per-server safety limit in JiraIssueSearch.cpp.
    constexpr int kMaxPlanePages = 50;
    std::string listCursor;

    while (true) {
        if (shouldCancel && shouldCancel()) {
            result.EndedCleanly = false;
            break;
        }

        if (result.PageCount >= kMaxPlanePages) {
            const std::string warn = "Plane pagination outer page cap (" + std::to_string(kMaxPlanePages) +
                                     ") reached; remaining issues not fetched. Narrow your view or raise the cap.";
            LOG_WARN("PlaneClient::FetchIssuesStreamed %s", warn.c_str());
            // Soft warning: the issues we did fetch are valid; treat as a partial success
            // rather than a fetch failure so the UI fires its success notify and the
            // connectivity banner stays clear.
            summary.Warning = warn;
            break;
        }
        ++result.PageCount;

        // Phase 4: fetch + classify one page. Any hard failure carries a ready error string.
        PlaneIssuePageFetch page =
            FetchPlaneIssuePage(planeApi, workspaceSlug, planeProjectId, headers, pageSize, listCursor, shouldCancel);
        if (!page.Ok) {
            LOG_ERROR("PlaneClient::FetchIssuesStreamed %s", page.Error.c_str());
            summary.FetchError = page.Error;
            result.HardFailed = true;
            return result;
        }

        auto results = (page.Body.is_object() && page.Body.contains("results")) ? page.Body["results"] : page.Body;

        if (results.empty()) {
            result.EndedCleanly = true;
            break;
        }

        // Phase 5: map this page's work-items into CachedTickets via the pure mapper, which
        // reproduces the per-issue field mapping (status / assignee / sprint / labels / type /
        // description-html) and silently skips rows that throw — parity with the old inline
        // try/catch. Fills localKeyToId with the visual-key → uuid associations.
        std::vector<CachedTicket> pageIssues = smatchet::plane::MapPlaneWorkItemsArrayToCachedTickets(
            results, projectIdentifier, userLookup, &localKeyToId);

        if (shouldCancel && shouldCancel()) {
            result.EndedCleanly = false;
            break;
        }

        size_t added = pageIssues.size();
        summary.FetchedCount += added;

        if (onBatch && added > 0) {
            onBatch(std::move(pageIssues));
        }

        // Phase 6: cursor pagination — advance or terminate.
        listCursor = smatchet::plane::NextPaginationCursor(page.Body);
        if (listCursor.empty()) {
            result.EndedCleanly = true;
            break;
        }
    }
    return result;
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
                                                          const ViewsStore* viewsOverride) {

    TrackerIssueFetchSummary summary;

    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();

    const std::string activeViewJql = ActiveViewJqlFromStore(viewsOverride);
    const std::string projectKey = smatchet::ResolvePlaneOperationProject(this, activeViewJql, cfg.JqlQuery);

    summary.FetchError = ValidatePlaneFetchConfig(cfg, projectKey);
    if (!summary.FetchError.empty()) {
        return summary;
    }

    // Plan user-info-window.md item 18b: a `key` member in the structured query narrows the
    // stream to one issue (client-side post-filter — Plane's list endpoint has no key filter).
    // Same match rule as FetchIssuesForKeys: UUID or visual key, exact.
    const std::string keyFilter = smatchet::plane::ExtractKeyFromPlaneQuery(cfg.JqlQuery);
    const BatchCallback filteredOnBatch = WrapPlaneKeyFilterBatch(keyFilter, onBatch);

    const std::string planeApi = NormalizeAndLogPlaneApiBase(cfg.PlaneUrl);

    const cpr::Header headers = ToCprHeader(BuildPlaneHeaders(cfg));

    std::string prevProjectId;
    std::string tempProjectId;
    std::string tempProjectIdentifier;
    std::vector<TrackerUser> localCachedUsers;
    {
        std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
        prevProjectId = planeProjectId_;
        tempProjectId = planeProjectId_;
        tempProjectIdentifier = planeProjectIdentifier_;
        localCachedUsers = cachedUsers_;
    }

    std::string resolveError;
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
    const std::string& planeProjectId = tempProjectId;
    std::unordered_map<std::string, std::string> localKeyToId;

    try {
        // Phase 3: fetch states for display value mapping. A 200 + parseable body commits even when
        // the parsed list is empty (clears stale states); non-200 / discarded leave the cache as-is.
        bool shouldCommitStates = false;
        std::vector<std::pair<std::string, std::string>> statePairs =
            FetchPlaneStatePairs(planeApi, cfg.PlaneWorkspaceSlug, planeProjectId, headers, shouldCommitStates);
        if (shouldCommitStates) {
            std::vector<CachedState> tempCachedStates = PairsToCached<CachedState>(statePairs);
            std::lock_guard<std::recursive_mutex> lock(planeCacheMutex_);
            cachedStates_ = std::move(tempCachedStates);
        }

        // Assignee-display lookup for the pure mapper: translate cached TrackerUsers into the
        // mapper's opaque (AccountId, DisplayName) pairs once, ahead of the page loop.
        const std::vector<smatchet::plane::UserDisplayLookup> userLookup = BuildPlaneUserLookups(localCachedUsers);

        // Phases 4-6: drive the cursor-paginated work-items fetch/map/stream loop.
        PlanePageLoopResult loop =
            RunPlanePageLoop(planeApi, cfg.PlaneWorkspaceSlug, planeProjectId, tempProjectIdentifier, headers,
                             userLookup, filteredOnBatch, shouldCancel, localKeyToId, summary);
        if (loop.HardFailed) {
            return summary;
        }
        summary.FullSyncCompleted = loop.EndedCleanly && loop.PageCount > 0 && (!shouldCancel || !shouldCancel());
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

Result<std::vector<CachedTicket>, TrackerError>
PlaneClient::FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                const ViewsStore& /*views*/) {
    using FetchResult = Result<std::vector<CachedTicket>, TrackerError>;
    std::vector<CachedTicket> outTickets;
    if (issueKeys.empty()) {
        return FetchResult::Ok(std::move(outTickets));
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
        // FetchIssuesStreamed surfaces only a string (no HTTP status); wrap Unknown with the Detail
        // preserved verbatim (the caller's IsTrackerTransportErrorText reads the text). TODO(#21b
        // later slice): thread a TrackerError through the streamed-fetch summary for .Kind.
        return FetchResult::Err(TrackerErrorUnknown(summary.FetchError));
    }
    return FetchResult::Ok(std::move(outTickets));
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
        std::string parseErr;
        const nlohmann::json j = smatchet::json_safe::ParseBounded(StripUtf8BomCopy(resp.text), parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("PlaneClient::ListProjects: invalid JSON in response: %s", parseErr.c_str());
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
