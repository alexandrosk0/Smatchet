#include "JiraClient.h"

#include "JiraCommentMappingPure.h"
#include "JiraIssueMappingPure.h"
#include "Tracker/JqlEscape.h"
#include "TrackerFieldValueParser.h"
#include "TrackerHttpUtils.h"
#include "JsonParseUtil.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

bool JiraFetchIssueCommentsPages(const std::string& base, const cpr::Header& headers, const std::string& issueKey,
                                 nlohmann::json& outComments) {
    outComments = nlohmann::json::array();
    int startAt = 0;
    const int maxResults = 100;
    const int maxPages = 20;

    for (int page = 0; page < maxPages; ++page) {
        const std::string commentsUrl = base + "/rest/api/3/issue/" + UrlEncode(issueKey) +
                                        "/comment?startAt=" + std::to_string(startAt) +
                                        "&maxResults=" + std::to_string(maxResults);
        auto commentsResp = TrackerGetLogged("JiraClient", commentsUrl, headers);
        if (commentsResp.status_code != 200) {
            LOG_WARN("JiraClient: failed to fetch comments for issue %s. HTTP %d", issueKey.c_str(),
                     commentsResp.status_code);
            // Signal failure on a mid-stream page error rather than returning the pages gathered
            // so far — a partial-Ok would silently truncate the thread shown in the modal.
            return false;
        }

        try {
            std::string parseErr;
            auto commentsJson = smatchet::json_safe::ParseBounded(commentsResp.text, parseErr);
            if (!parseErr.empty()) {
                LOG_WARN("JiraClient: failed to parse comments for issue %s: %s", issueKey.c_str(), parseErr.c_str());
                return false;
            }
            if (!commentsJson.contains("comments") || !commentsJson["comments"].is_array()) {
                LOG_WARN("JiraClient: comments endpoint for %s missing comments array.", issueKey.c_str());
                return false;
            }

            const auto& pageComments = commentsJson["comments"];
            std::copy(pageComments.begin(), pageComments.end(), std::back_inserter(outComments));

            const int total = commentsJson.value("total", static_cast<int>(outComments.size()));
            const int pageCount = static_cast<int>(pageComments.size());
            const int reportedMaxResults = commentsJson.value("maxResults", pageCount);
            startAt += (reportedMaxResults > 0 ? reportedMaxResults : pageCount);

            if (static_cast<int>(outComments.size()) >= total || pageCount == 0) {
                break;
            }
        } catch (const std::exception& ex) {
            LOG_WARN("JiraClient: failed to parse comments for issue %s: %s", issueKey.c_str(), ex.what());
            return false;
        }
    }
    return true;
}

// Outcome of parsing + mapping one Jira search page. `Stop` ends the pagination loop; `Token`
// carries the next-page cursor when the loop should continue. Mirrors the original inline
// control-flow byte-for-byte (isLast / empty-token / parse-error early exits).
struct JiraSearchPageOutcome {
    bool Stop = false;          // terminate the pagination loop after this page
    bool EndedCleanly = false;  // value to assign to syncEndedCleanly
    bool HadFetchError = false; // when set, FetchError carries the message
    std::string FetchError;     // populated only when HadFetchError
    std::string NextToken;      // next-page cursor (valid only when !Stop)
};

// Phase: parse one search-page body, map its issues onto `summary` + the batch callback, and
// classify pagination. Reproduces the original inline per-page block exactly — including the
// per-issue map-failure logging, the cancel checks, and the isLast / empty-token branches.
JiraSearchPageOutcome
ProcessJiraSearchPage(int page, const std::string& responseText, const std::string& lastResponseBody,
                      const std::vector<std::string>& selectedFields,
                      const std::function<bool(const std::string&, nlohmann::json&)>& fetchIssueComments,
                      const JiraClient::BatchCallback& onBatch, const JiraClient::CancelCallback& shouldCancel,
                      TrackerIssueFetchSummary& summary) {
    JiraSearchPageOutcome outcome;
    try {
        std::string parseErr;
        auto json = smatchet::json_safe::ParseBounded(responseText, parseErr);
        if (!parseErr.empty()) {
            LOG_ERROR("JiraClient: JSON parse error on page %d: %s | response excerpt: %s", page, parseErr.c_str(),
                      RedactHttpBodyForLog(lastResponseBody).c_str());
            outcome.Stop = true;
            outcome.EndedCleanly = false;
            outcome.HadFetchError = true;
            // Parser detail stays in the log line above; the banner shows actionable text.
            outcome.FetchError = "the Tracker returned an unreadable response (invalid JSON)";
            return outcome;
        }
        if (!json.contains("issues")) {
            LOG_WARN("JiraClient: page %d response has no 'issues' key. Body (truncated):\n%s", page,
                     RedactHttpBodyForLog(responseText).c_str());
            outcome.Stop = true;
            outcome.EndedCleanly = false;
            outcome.HadFetchError = true;
            outcome.FetchError = "Jira search response missing 'issues' array.";
            return outcome;
        }

        std::vector<CachedTicket> pageTickets;
        for (const auto& issue : json["issues"]) {
            if (shouldCancel && shouldCancel()) {
                break;
            }
            if (!smatchet::jira::AppendCachedTicketFromJiraSearchIssue(issue, selectedFields, fetchIssueComments,
                                                                       pageTickets)) {
                const std::string issueKey = issue.is_object() ? JsonGetStringIfString(issue, "key") : std::string();
                LOG_ERROR("JiraClient: JSON error on page %d while parsing issue %s", page,
                          issueKey.empty() ? "(unknown key)" : issueKey.c_str());
                outcome.EndedCleanly = false;
            }
        }

        if (shouldCancel && shouldCancel()) {
            outcome.Stop = true;
            outcome.EndedCleanly = false;
            return outcome;
        }

        size_t added = pageTickets.size();
        summary.FetchedCount += added;
        LOG_INFO("JiraClient: fetched page %d with %zu issues (total=%zu).", page, added, summary.FetchedCount);

        if (onBatch && added > 0) {
            onBatch(std::move(pageTickets));
        }

        const bool isLast = json.value("isLast", true);
        std::string newToken = JsonGetStringIfString(json, "nextPageToken");
        if (json.contains("nextPageToken") && !json["nextPageToken"].is_null() && !json["nextPageToken"].is_string()) {
            LOG_WARN("JiraClient: page %d nextPageToken is not a string (JSON type: %s); pagination may stop.", page,
                     json["nextPageToken"].type_name());
        }
        if (isLast) {
            outcome.Stop = true;
            outcome.EndedCleanly = true;
            return outcome;
        }
        if (newToken.empty()) {
            LOG_WARN("JiraClient: page %d indicates more pages but nextPageToken is empty. Stopping pagination.", page);
            outcome.Stop = true;
            outcome.EndedCleanly = false;
            return outcome;
        }
        outcome.NextToken = std::move(newToken);
    } catch (const std::exception& ex) {
        LOG_ERROR("JiraClient: JSON parse error on page %d: %s | response excerpt: %s", page, ex.what(),
                  RedactHttpBodyForLog(lastResponseBody).c_str());
        outcome.Stop = true;
        outcome.EndedCleanly = false;
        outcome.HadFetchError = true;
        outcome.FetchError = "the Tracker returned an unreadable response (invalid JSON)";
    }
    return outcome;
}

// Phase: when a 200-OK sync yields zero issues with no error, confirm which account the API sees.
// Distinguishes authenticated-vs-anonymous 200 responses. Logs a WARN; no state mutation.
void JiraDiagnoseZeroIssueResult(const TrackerConfig& cfg, const std::string& base, const cpr::Header& headers,
                                 int fetchedPages) {
    std::string who = cfg.Email;
    bool verifiedIdentity = false;
    std::string myselfUrl = base + "/rest/api/3/myself";
    auto myselfResp = TrackerGetLogged("JiraClient", myselfUrl, headers);
    if (myselfResp.status_code == 200) {
        try {
            std::string parseErr;
            auto me = smatchet::json_safe::ParseBounded(myselfResp.text, parseErr);
            if (!parseErr.empty()) {
                LOG_WARN("JiraClient: failed to parse /myself response: %s body=%s", parseErr.c_str(),
                         RedactHttpBodyForLog(myselfResp.text).c_str());
            } else {
                std::string name = me.value("displayName", std::string());
                std::string emailAddr = me.value("emailAddress", std::string());
                who = name.empty() ? emailAddr : (name + " <" + emailAddr + ">");
                verifiedIdentity = true;
            }
        } catch (const std::exception& ex) {
            LOG_WARN("JiraClient: failed to parse /myself response: %s body=%s", ex.what(),
                     RedactHttpBodyForLog(myselfResp.text).c_str());
        }
    }

    if (verifiedIdentity) {
        LOG_WARN("JiraClient: API returned 0 issues for your JQL after %d page(s). Verified identity: %s. "
                 "If the same URL in the browser shows issues, compare browser account/permissions with this API "
                 "identity.",
                 fetchedPages, who.c_str());
    } else {
        LOG_WARN("JiraClient: API returned 0 issues and /myself was not authenticated (HTTP %d). "
                 "This usually means the app sent invalid credentials (often a truncated API token). "
                 "Re-open Settings → Preferences → Jira and paste the full API token.",
                 myselfResp.status_code);
    }
}

// Trim leading/trailing spaces + tabs so JQL matches what you'd paste in the
// browser. Returns the trimmed copy.
std::string TrimJqlWhitespace(const std::string& raw) {
    std::string jqlRaw = raw;
    while (!jqlRaw.empty() && (jqlRaw.front() == ' ' || jqlRaw.front() == '\t'))
        jqlRaw.erase(0, 1);
    while (!jqlRaw.empty() && (jqlRaw.back() == ' ' || jqlRaw.back() == '\t'))
        jqlRaw.pop_back();
    return jqlRaw;
}

// Build the JQL key clause for the requested keys: a single equality match for
// one key, or an "in" list for several. Reads count keys from the given offset.
std::string BuildKeyInJql(const std::vector<std::string>& keys, std::size_t offset, std::size_t count) {
    // Issue keys come from the tracker server, so JQL-escape each through
    // tracker_jql::QuoteLiteral before adding the surrounding quotes — a `"` or `\`
    // in a key is escaped, never allowed to break out of the literal (security: E1).
    if (count == 1) {
        return "key = \"" + tracker_jql::QuoteLiteral(keys[offset]) + "\"";
    }
    std::string jql = "key in (";
    for (std::size_t i = 0; i < count; ++i) {
        if (i) {
            jql += ',';
        }
        jql += '"';
        jql += tracker_jql::QuoteLiteral(keys[offset + i]);
        jql += '"';
    }
    jql += ')';
    return jql;
}

// Log a non-200 page response and return the summary fetch-error string.
std::string LogAndBuildPageFetchError(int page, const cpr::Response& response) {
    LOG_ERROR("JiraClient: failed to fetch issues page %d. HTTP %d, error code %d.", page, response.status_code,
              static_cast<int>(response.error.code));
    if (response.status_code == 401 || response.status_code == 403 ||
        (response.text.find("authenticated") != std::string::npos)) {
        LOG_WARN("JiraClient: login failed. Check Email and API token under Settings → Preferences → Jira.");
    }
    LOG_DEBUG("JiraClient: response error message: %s", response.error.message.c_str());
    std::string msg = "HTTP " + std::to_string(response.status_code);
    // Promote the auth hint into the user-facing summary — it was log-only, leaving the
    // banner a bare status code with no next step on the most common failure.
    if (response.status_code == 401 || response.status_code == 403) {
        msg += " — authentication failed. Check Email and API token under Settings -> Preferences -> Tracker.";
    } else if (!response.error.message.empty()) {
        msg += std::string(" ") + response.error.message;
    }
    return msg;
}

// Deduplicate + trim issue keys, preserving first-seen order.
std::vector<std::string> DedupeIssueKeys(const std::vector<std::string>& issueKeys) {
    std::vector<std::string> keys;
    keys.reserve(issueKeys.size());
    std::unordered_set<std::string> seen;
    for (const auto& k : issueKeys) {
        const std::string t = TrimCopy(k);
        if (t.empty() || !seen.insert(t).second) {
            continue;
        }
        keys.push_back(t);
    }
    return keys;
}

} // namespace

std::vector<CachedTicket> JiraClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
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

TrackerIssueFetchSummary JiraClient::FetchIssuesStreamed(const BatchCallback& onBatch,
                                                         const CancelCallback& shouldCancel,
                                                         const TrackerConfig* configOverride,
                                                         const ViewsStore* viewsOverride) {

    TrackerIssueFetchSummary summary;

    TrackerConfig cfgStorage;
    const TrackerConfig* cfgPtr = configOverride;
    if (!cfgPtr) {
        cfgStorage = ConfigManager::Load();
        cfgPtr = &cfgStorage;
    }
    const TrackerConfig& cfg = *cfgPtr;

    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        LOG_WARN("JiraClient: missing API token or domain; skipping FetchIssues.");
        summary.FetchError = "Missing Tracker domain or API token.";
        return summary;
    }

    std::string base = NormalizeBaseUrl(cfg.Domain);

    const std::string jqlRaw =
        TrimJqlWhitespace(cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery);
    const std::string jqlEncoded = UrlEncode(jqlRaw);

    // Prefer the active view's field list as the single source of truth.
    ViewsStore viewsStorage;
    const ViewsStore* viewsPtr = viewsOverride;
    if (!viewsPtr) {
        viewsStorage = ConfigManager::LoadViewsOrBootstrap(cfg);
        viewsPtr = &viewsStorage;
    }
    const ViewsStore& viewStore = *viewsPtr;

    std::vector<std::string> fieldsList;
    std::vector<std::string> selectedFields;
    smatchet::jira::BuildFetchFieldListsFromView(viewStore, fieldsList, selectedFields);
    const std::string fields = JoinStrings(fieldsList, ",");

    // Use the new Jira Cloud search endpoint as per Atlassian migration guidance.
    std::string baseSearchUrl =
        base + "/rest/api/3/search/jql?jql=" + jqlEncoded + "&maxResults=100&fields=" + fields + "&expand=changelog";

    const cpr::Header headers = BuildTrackerHeaders(cfg);
    auto fetchIssueComments = [&](const std::string& issueKey, nlohmann::json& outComments) -> bool {
        return JiraFetchIssueCommentsPages(base, headers, issueKey, outComments);
    };

    std::string nextPageToken;
    const int kMaxPages = 50;
    int fetchedPages = 0;
    bool syncEndedCleanly = false;

    for (int page = 1; page <= kMaxPages; ++page) {
        if (shouldCancel && shouldCancel()) {
            syncEndedCleanly = false;
            break;
        }

        std::string pageUrl = baseSearchUrl;
        if (!nextPageToken.empty()) {
            pageUrl += "&nextPageToken=" + UrlEncode(nextPageToken);
        }
        LOG_DEBUG("JiraClient: fetching issues page %d from URL: %s", page, pageUrl.c_str());

        // Thread the sync worker's cancellation token into the retry loop so an abort during a
        // backoff/retry window is honoured immediately, not only at the next page boundary.
        auto response = TrackerGetLogged("JiraClient", pageUrl, headers, shouldCancel);
        const std::string lastResponseBody = response.text;
        if (response.status_code != 200) {
            summary.FetchError = LogAndBuildPageFetchError(page, response);
            break;
        }

        fetchedPages++;
        JiraSearchPageOutcome outcome = ProcessJiraSearchPage(page, response.text, lastResponseBody, selectedFields,
                                                              fetchIssueComments, onBatch, shouldCancel, summary);
        if (outcome.HadFetchError) {
            summary.FetchError = std::move(outcome.FetchError);
        }
        if (outcome.Stop) {
            syncEndedCleanly = outcome.EndedCleanly;
            break;
        }
        nextPageToken = std::move(outcome.NextToken);
    }

    if (fetchedPages >= kMaxPages && !nextPageToken.empty()) {
        LOG_WARN("JiraClient: reached pagination safety limit (%d pages). Results may be incomplete.", kMaxPages);
    }

    summary.FullSyncCompleted = syncEndedCleanly && fetchedPages > 0 && (!shouldCancel || !shouldCancel());

    if (summary.FetchedCount == 0 && summary.FetchError.empty()) {
        JiraDiagnoseZeroIssueResult(cfg, base, headers, fetchedPages);
    }

    return summary;
}

Result<std::vector<CachedTicket>, TrackerError>
JiraClient::FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                               const ViewsStore& viewStore) {
    using FetchResult = Result<std::vector<CachedTicket>, TrackerError>;
    std::vector<CachedTicket> outTickets;
    std::string outError;
    if (issueKeys.empty()) {
        return FetchResult::Ok(std::move(outTickets));
    }
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return FetchResult::Err(TrackerErrorAuth(outError));
    }

    const std::vector<std::string> keys = DedupeIssueKeys(issueKeys);
    if (keys.empty()) {
        return FetchResult::Ok(std::move(outTickets));
    }

    std::vector<std::string> fieldsList;
    std::vector<std::string> selectedFields;
    smatchet::jira::BuildFetchFieldListsFromView(viewStore, fieldsList, selectedFields);
    const std::string fields = JoinStrings(fieldsList, ",");

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);
    auto fetchIssueComments = [&](const std::string& issueKey, nlohmann::json& outComments) -> bool {
        return JiraFetchIssueCommentsPages(base, headers, issueKey, outComments);
    };

    constexpr std::size_t kMaxKeysPerRequest = 40;
    for (size_t offset = 0; offset < keys.size(); offset += kMaxKeysPerRequest) {
        const size_t n = (std::min)(kMaxKeysPerRequest, keys.size() - offset);
        const std::string jql = BuildKeyInJql(keys, offset, n);
        const std::string jqlEncoded = UrlEncode(jql);
        const std::string pageUrl =
            base + "/rest/api/3/search/jql?jql=" + jqlEncoded +
            // SMATCHET_DEVIATION(rule=duplication; reason=pre-existing clone; owner=security-audit; revisit=2026-09-30)
            "&maxResults=" + std::to_string(n) + "&fields=" + fields + "&expand=changelog";

        auto response = TrackerGetLogged("JiraClient", pageUrl, headers);
        if (response.status_code != 200) {
            outError = "Fetch by key failed: HTTP " + std::to_string(response.status_code);
            // SMATCHET_DEVIATION(rule=duplication; reason=pre-existing clone; owner=security-audit; revisit=2026-09-30)
            LOG_WARN("JiraClient::FetchIssuesForKeys: %s", outError.c_str());
            // Guard the `!= 200` branch before FromHttpStatus: a 2xx-other (201/204) would map to
            // Ok() and yield an Err(Kind::None) with empty Detail (FIX-1 / Slice-2). Detail is
            // preserved verbatim for the caller's IsTrackerTransportErrorText text check.
            if (response.status_code >= 200 && response.status_code < 300) {
                return FetchResult::Err(TrackerErrorUnknown(outError, response.status_code));
            }
            return FetchResult::Err(TrackerErrorFromHttpStatus(response.status_code, outError));
        }
        try {
            std::string parseErr;
            auto json = smatchet::json_safe::ParseBounded(response.text, parseErr);
            if (!parseErr.empty()) {
                LOG_ERROR("JiraClient: fetch-by-key parse failed: %s", parseErr.c_str());
                outError = "The Tracker returned an unreadable response for this issue — try again.";
                return FetchResult::Err(TrackerErrorParse(outError));
            }
            if (!json.contains("issues") || !json["issues"].is_array()) {
                outError = "Fetch by key: response missing issues array.";
                return FetchResult::Err(TrackerErrorParse(outError));
            }
            std::unordered_set<std::string> fetchedKeys;
            for (const auto& issue : json["issues"]) {
                if (issue.is_object()) {
                    const std::string fetchedKey = JsonGetStringIfString(issue, "key");
                    if (!fetchedKey.empty()) {
                        fetchedKeys.insert(fetchedKey);
                    }
                }
                if (!smatchet::jira::AppendCachedTicketFromJiraSearchIssue(issue, selectedFields, fetchIssueComments,
                                                                           outTickets)) {
                    const std::string issueKey =
                        issue.is_object() ? JsonGetStringIfString(issue, "key") : std::string();
                    LOG_WARN("JiraClient::FetchIssuesForKeys: failed to parse issue %s",
                             issueKey.empty() ? "(unknown)" : issueKey.c_str());
                }
            }
            if (n == 1) {
                const std::string& requestedKey = keys[offset];
                if (!requestedKey.empty() && fetchedKeys.count(requestedKey) == 0) {
                    const std::string issueUrl = base + "/rest/api/3/issue/" + UrlEncode(requestedKey) +
                                                 "?fields=" + fields + "&expand=changelog";
                    auto issueResp = TrackerGetLogged("JiraClient", issueUrl, headers);
                    if (issueResp.status_code == 200) {
                        std::string issueParseErr;
                        auto issueJson = smatchet::json_safe::ParseBounded(issueResp.text, issueParseErr);
                        if (issueParseErr.empty()) {
                            // Best-effort fallback hydration only: a throw from
                            // AppendCachedTicketFromJiraSearchIssue must stay local (skip this
                            // one issue) rather than propagate to the outer catch and abort the
                            // whole FetchIssuesForKeys — preserves the pre-sweep inner-catch.
                            try {
                                (void)smatchet::jira::AppendCachedTicketFromJiraSearchIssue(
                                    issueJson, selectedFields, fetchIssueComments, outTickets);
                            } catch (const std::exception&) { // catch-all-ok: best-effort fallback hydration
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("JiraClient: fetch-by-key parse failed: %s", ex.what());
            outError = "The Tracker returned an unreadable response for this issue — try again.";
            return FetchResult::Err(TrackerErrorParse(outError));
        }
    }
    return FetchResult::Ok(std::move(outTickets));
}

Result<std::vector<TrackerIssueComment>, TrackerError> JiraClient::FetchIssueComments(const std::string& issueKey) {
    using CommentsResult = Result<std::vector<TrackerIssueComment>, TrackerError>;
    // Reject an empty key before any network call — otherwise the URL builder emits
    // `/issue//comment`, returning transport noise instead of a deterministic error.
    if (issueKey.empty()) {
        return CommentsResult::Err(TrackerErrorInvalidRequest("JiraClient::FetchIssueComments: empty issue key"));
    }
    // No cfg parameter on this interface — resolve from the settled on-disk config
    // (mirrors GitHubClient::FetchIssueComments / the cfg-less Jira read pattern).
    const TrackerConfig cfg = ConfigManager::Load();
    std::string outError;
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return CommentsResult::Err(TrackerErrorAuth(outError));
    }
    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    nlohmann::json nodes = nlohmann::json::array();
    if (!JiraFetchIssueCommentsPages(base, headers, issueKey, nodes)) {
        return CommentsResult::Err(
            TrackerErrorUnknown("JiraClient::FetchIssueComments: comment fetch failed for " + issueKey, 0));
    }

    std::vector<TrackerIssueComment> all = smatchet::jira::MapJiraIssueComments(nodes);
    LOG_INFO("JiraClient::FetchIssueComments: %s → %zu comments", issueKey.c_str(), all.size());
    return CommentsResult::Ok(std::move(all));
}
