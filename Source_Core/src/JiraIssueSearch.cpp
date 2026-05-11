#include "JiraClient.h"

#include "TrackerFieldValueParser.h"
#include "TrackerHttpUtils.h"
#include "JsonParseUtil.h"
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
            return !outComments.empty();
        }

        try {
            auto commentsJson = nlohmann::json::parse(commentsResp.text);
            if (!commentsJson.contains("comments") || !commentsJson["comments"].is_array()) {
                LOG_WARN("JiraClient: comments endpoint for %s missing comments array.", issueKey.c_str());
                return !outComments.empty();
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
            return !outComments.empty();
        }
    }
    return true;
}

void BuildFetchFieldListsFromView(const ViewsStore& viewStore, std::vector<std::string>& outFieldsList,
                                  std::vector<std::string>& outSelectedFields) {
    outFieldsList = std::vector<std::string>{"summary",   "description", "status",  "assignee", "priority",
                                             "issuetype", "parent",      "comment", "changelog"};
    std::unordered_set<std::string> seenFields(outFieldsList.begin(), outFieldsList.end());
    outSelectedFields.clear();
    std::unordered_set<std::string> seenSelectedFields;

    const ViewDefinition* activeViewDef = nullptr;
    auto vIt = std::find_if(viewStore.Views.begin(), viewStore.Views.end(),
                            [&](const auto& view) { return view.Id == viewStore.ActiveViewId; });
    if (vIt != viewStore.Views.end()) {
        activeViewDef = &(*vIt);
    }
    if (!activeViewDef && !viewStore.Views.empty()) {
        activeViewDef = &viewStore.Views.front();
    }
    if (activeViewDef) {
        for (const auto& rawField : activeViewDef->Fields) {
            const std::string field = TrimCopy(rawField);
            if (field.empty()) {
                continue;
            }
            if (field == "history") {
                if (seenSelectedFields.insert(field).second) {
                    outSelectedFields.push_back(field);
                }
                continue;
            }
            if (!seenSelectedFields.insert(field).second) {
                continue;
            }
            if (seenFields.insert(field).second) {
                outFieldsList.push_back(field);
                outSelectedFields.push_back(field);
            } else {
                outSelectedFields.push_back(field);
            }
        }
    }
    if (outSelectedFields.empty()) {
        const char* kBasicDefaults[] = {
            "summary", "assignee", "priority", "status", "created", "updated",
        };
        for (const char* basicId : kBasicDefaults) {
            if (seenFields.insert(basicId).second) {
                outFieldsList.push_back(basicId);
            }
            if (seenSelectedFields.insert(basicId).second) {
                outSelectedFields.emplace_back(basicId);
            }
        }
    }
}

bool JiraAppendCachedTicketFromSearchIssue(
    const nlohmann::json& issue, const std::vector<std::string>& selectedFields,
    const std::function<bool(const std::string&, nlohmann::json&)>& fetchIssueComments,
    std::vector<CachedTicket>& results) {
    try {
        CachedTicket ticket;
        ticket.id = JsonGetStringIfString(issue, "key");

        nlohmann::json issueFields = issue.value("fields", nlohmann::json::object());
        const auto stringifyForGrid = [&](const nlohmann::json& v) -> std::string {
            if (v.is_object() || v.is_array()) {
                return v.dump();
            }
            return NormalizeTrackerFieldValue(v);
        };

        for (const auto& fieldKey : selectedFields) {
            if (fieldKey == "history") {
                const nlohmann::json changelog = issue.value("changelog", nlohmann::json::object());
                const nlohmann::json histories = changelog.value("histories", nlohmann::json::array());
                ticket.fieldValues["history"] = ParseChangelog(histories);
                continue;
            }
            if (fieldKey == "watchers") {
                if (issueFields.contains("watchers")) {
                    ticket.fieldValues["watchers"] = stringifyForGrid(issueFields["watchers"]);
                } else if (issueFields.contains("watches")) {
                    ticket.fieldValues["watchers"] = stringifyForGrid(issueFields["watches"]);
                } else {
                    ticket.fieldValues["watchers"] = std::string();
                }
                continue;
            }
            if (issueFields.contains(fieldKey)) {
                const auto& rawValue = issueFields[fieldKey];
                if (fieldKey == "comment" && rawValue.is_object() && rawValue.contains("comments")) {
                    const auto& commentObj = rawValue;
                    const auto& commentsArray = commentObj["comments"];
                    const int totalComments = commentObj.value("total", static_cast<int>(commentsArray.size()));
                    if (commentsArray.is_array() && !commentsArray.empty()) {
                        ticket.fieldValues[fieldKey] = ParseComments(commentsArray);
                    } else if (totalComments > 0) {
                        nlohmann::json fetchedComments = nlohmann::json::array();
                        if (fetchIssueComments(ticket.id, fetchedComments) && fetchedComments.is_array()) {
                            ticket.fieldValues[fieldKey] = ParseComments(fetchedComments);
                        } else {
                            ticket.fieldValues[fieldKey] = ParseComments(commentsArray);
                        }
                    } else {
                        ticket.fieldValues[fieldKey] = std::string();
                    }
                } else if (fieldKey == "timetracking" || fieldKey == "aggregatetimetracking") {
                    if (rawValue.is_object()) {
                        ticket.fieldValues[fieldKey] = FormatTrackerTimetrackingDisplay(rawValue);
                    } else {
                        ticket.fieldValues[fieldKey] = NormalizeTrackerFieldValue(rawValue);
                    }
                } else if (fieldKey == "attachment" || fieldKey == "attachments") {
                    ticket.fieldValues[fieldKey] = stringifyForGrid(rawValue);
                } else if (fieldKey == "timeoriginalestimate" || fieldKey == "timeestimate" ||
                           fieldKey == "timespent" || fieldKey == "aggregatetimeoriginalestimate" ||
                           fieldKey == "aggregatetimeestimate" || fieldKey == "aggregatetimespent") {
                    long long seconds = 0;
                    if (rawValue.is_number_integer()) {
                        seconds = rawValue.get<long long>();
                    } else if (rawValue.is_number_unsigned()) {
                        seconds = static_cast<long long>(rawValue.get<unsigned long long>());
                    }
                    ticket.fieldValues[fieldKey] = FormatWorkDurationFromSeconds(seconds);
                } else {
                    ticket.fieldValues[fieldKey] = NormalizeTrackerFieldValue(rawValue);
                }
                // Capture the original ADF document alongside the stripped text so the long-text
                // modal editor can round-trip without format loss. Jira description / environment
                // / any custom field returning an ADF "doc" object hits this path. See
                // RICH_TEXT_EDITING_V2_PLAN.md.
                if (rawValue.is_object() && rawValue.value("type", std::string()) == "doc") {
                    ticket.fieldRichValues[fieldKey] = rawValue.dump();
                }
            } else {
                ticket.fieldValues[fieldKey] = std::string();
            }
        }
        if (issueFields.contains("issuetype")) {
            ticket.fieldValues["issuetype"] = NormalizeTrackerFieldValue(issueFields["issuetype"]);
        }
        results.push_back(std::move(ticket));
        return true;
    } catch (const std::exception&) {
        return false;
    }
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

TrackerIssueFetchSummary JiraClient::FetchIssuesStreamed(
    const BatchCallback& onBatch,
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

    std::string jqlRaw = cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery;
    // Trim leading/trailing whitespace so JQL matches what you'd paste in the browser.
    while (!jqlRaw.empty() && (jqlRaw.front() == ' ' || jqlRaw.front() == '\t'))
        jqlRaw.erase(0, 1);
    while (!jqlRaw.empty() && (jqlRaw.back() == ' ' || jqlRaw.back() == '\t'))
        jqlRaw.pop_back();
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
    BuildFetchFieldListsFromView(viewStore, fieldsList, selectedFields);
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

        auto response = TrackerGetLogged("JiraClient", pageUrl, headers);
        const std::string lastResponseBody = response.text;
        if (response.status_code != 200) {
            LOG_ERROR("JiraClient: failed to fetch issues page %d. HTTP %d, error code %d.", page, response.status_code,
                      static_cast<int>(response.error.code));
            if (response.status_code == 401 || response.status_code == 403 ||
                (response.text.find("authenticated") != std::string::npos)) {
                LOG_WARN("JiraClient: login failed. Check Email and API token under Settings → Preferences → Jira.");
            }
            LOG_DEBUG("JiraClient: response error message: %s", response.error.message.c_str());
            {
                std::string msg = "HTTP " + std::to_string(response.status_code);
                if (!response.error.message.empty()) {
                    msg += std::string(" ") + response.error.message;
                }
                summary.FetchError = std::move(msg);
            }
            break;
        }

        fetchedPages++;
        try {
            auto json = nlohmann::json::parse(response.text);
            if (!json.contains("issues")) {
                LOG_WARN("JiraClient: page %d response has no 'issues' key. Body (truncated):\n%s", page,
                         TruncateForLog(response.text, 800).c_str());
                syncEndedCleanly = false;
                summary.FetchError = "Jira search response missing 'issues' array.";
                break;
            }

            std::vector<CachedTicket> pageTickets;
            for (const auto& issue : json["issues"]) {
                if (shouldCancel && shouldCancel()) {
                    break;
                }
                if (!JiraAppendCachedTicketFromSearchIssue(issue, selectedFields, fetchIssueComments, pageTickets)) {
                    const std::string issueKey =
                        issue.is_object() ? JsonGetStringIfString(issue, "key") : std::string();
                    LOG_ERROR("JiraClient: JSON error on page %d while parsing issue %s", page,
                              issueKey.empty() ? "(unknown key)" : issueKey.c_str());
                    syncEndedCleanly = false;
                }
            }

            if (shouldCancel && shouldCancel()) {
                syncEndedCleanly = false;
                break;
            }

            size_t added = pageTickets.size();
            summary.FetchedCount += added;
            LOG_INFO("JiraClient: fetched page %d with %zu issues (total=%zu).", page, added, summary.FetchedCount);

            if (onBatch && added > 0) {
                onBatch(std::move(pageTickets));
            }

            const bool isLast = json.value("isLast", true);
            std::string newToken = JsonGetStringIfString(json, "nextPageToken");
            if (json.contains("nextPageToken") && !json["nextPageToken"].is_null() &&
                !json["nextPageToken"].is_string()) {
                LOG_WARN("JiraClient: page %d nextPageToken is not a string (JSON type: %s); pagination may stop.",
                         page, json["nextPageToken"].type_name());
            }
            if (isLast) {
                syncEndedCleanly = true;
                break;
            }
            if (newToken.empty()) {
                LOG_WARN("JiraClient: page %d indicates more pages but nextPageToken is empty. Stopping pagination.",
                         page);
                syncEndedCleanly = false;
                break;
            }
            nextPageToken = newToken;
        } catch (const std::exception& ex) {
            LOG_ERROR("JiraClient: JSON parse error on page %d: %s | response excerpt: %s", page, ex.what(),
                      TruncateForLog(lastResponseBody).c_str());
            syncEndedCleanly = false;
            summary.FetchError = std::string("JSON parse error: ") + ex.what();
            break;
        }
    }

    if (fetchedPages >= kMaxPages && !nextPageToken.empty()) {
        LOG_WARN("JiraClient: reached pagination safety limit (%d pages). Results may be incomplete.", kMaxPages);
    }

    summary.FullSyncCompleted = syncEndedCleanly && fetchedPages > 0 && (!shouldCancel || !shouldCancel());

    if (summary.FetchedCount == 0 && summary.FetchError.empty()) {
        // Confirm which account the API sees. This distinguishes authenticated-vs-anonymous 200 responses.
        std::string who = cfg.Email;
        bool verifiedIdentity = false;
        std::string myselfUrl = base + "/rest/api/3/myself";
        auto myselfResp = TrackerGetLogged("JiraClient", myselfUrl, headers);
        if (myselfResp.status_code == 200) {
            try {
                auto me = nlohmann::json::parse(myselfResp.text);
                std::string name = me.value("displayName", std::string());
                std::string emailAddr = me.value("emailAddress", std::string());
                who = name.empty() ? emailAddr : (name + " <" + emailAddr + ">");
                verifiedIdentity = true;
            } catch (const std::exception& ex) {
                LOG_WARN("JiraClient: failed to parse /myself response: %s body=%s", ex.what(),
                         TruncateForLog(myselfResp.text, 300).c_str());
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

    return summary;
}


bool JiraClient::FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                    const ViewsStore& viewStore, std::vector<CachedTicket>& outTickets,
                                    std::string& outError) {
    outError.clear();
    if (issueKeys.empty()) {
        return true;
    }
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }

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
    if (keys.empty()) {
        return true;
    }

    std::vector<std::string> fieldsList;
    std::vector<std::string> selectedFields;
    BuildFetchFieldListsFromView(viewStore, fieldsList, selectedFields);
    const std::string fields = JoinStrings(fieldsList, ",");

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);
    auto fetchIssueComments = [&](const std::string& issueKey, nlohmann::json& outComments) -> bool {
        return JiraFetchIssueCommentsPages(base, headers, issueKey, outComments);
    };

    constexpr std::size_t kMaxKeysPerRequest = 40;
    for (size_t offset = 0; offset < keys.size(); offset += kMaxKeysPerRequest) {
        const size_t n = (std::min)(kMaxKeysPerRequest, keys.size() - offset);
        std::string jql;
        if (n == 1) {
            jql = "key = \"" + keys[offset] + "\"";
        } else {
            jql = "key in (";
            for (size_t i = 0; i < n; ++i) {
                if (i) {
                    jql += ',';
                }
                jql += '"';
                jql += keys[offset + i];
                jql += '"';
            }
            jql += ')';
        }
        const std::string jqlEncoded = UrlEncode(jql);
        const std::string pageUrl = base + "/rest/api/3/search/jql?jql=" + jqlEncoded +
                                    "&maxResults=" + std::to_string(n) + "&fields=" + fields + "&expand=changelog";

        auto response = TrackerGetLogged("JiraClient", pageUrl, headers);
        if (response.status_code != 200) {
            outError = "Fetch by key failed: HTTP " + std::to_string(response.status_code);
            LOG_WARN("JiraClient::FetchIssuesForKeys: %s", outError.c_str());
            return false;
        }
        try {
            auto json = nlohmann::json::parse(response.text);
            if (!json.contains("issues") || !json["issues"].is_array()) {
                outError = "Fetch by key: response missing issues array.";
                return false;
            }
            std::unordered_set<std::string> fetchedKeys;
            for (const auto& issue : json["issues"]) {
                if (issue.is_object()) {
                    const std::string fetchedKey = JsonGetStringIfString(issue, "key");
                    if (!fetchedKey.empty()) {
                        fetchedKeys.insert(fetchedKey);
                    }
                }
                if (!JiraAppendCachedTicketFromSearchIssue(issue, selectedFields, fetchIssueComments, outTickets)) {
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
                        try {
                            auto issueJson = nlohmann::json::parse(issueResp.text);
                            (void)JiraAppendCachedTicketFromSearchIssue(issueJson, selectedFields, fetchIssueComments,
                                                                        outTickets);
                        } catch (const std::exception&) {
                            // Best-effort fallback hydration only.
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            outError = std::string("Fetch by key parse error: ") + ex.what();
            return false;
        }
    }
    return true;
}






