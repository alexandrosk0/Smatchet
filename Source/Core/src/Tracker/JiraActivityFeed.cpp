#include "JiraActivityFeed.h"

#include "JiraClient.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "StringUtil.h"
#include "Tracker/JqlEscape.h"
#include "TrackerHttpUtils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace JiraActivityFeed {

namespace {

bool IsAsciiDigitChar(char c) { return c >= '0' && c <= '9'; }

/// Jira changelog estimate fields carry raw seconds in `from`/`to`.
bool IsEstimateLikeField(const std::string& field) {
    const std::string lower = ToLowerAsciiCopy(field);
    return lower.find("estimate") != std::string::npos || lower == "timespent";
}

std::string JsonValueAsString(const nlohmann::json& node, const char* key) {
    if (!node.contains(key)) {
        return std::string();
    }
    const auto& v = node[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    return std::string();
}

/// Human side of a from/to pair: prefer the display string; raw estimate seconds get
/// the workday formatter; anything else passes through raw.
std::string DisplaySide(const std::string& field, const std::string& raw, const std::string& display) {
    if (!display.empty()) {
        return display;
    }
    if (!raw.empty() && IsEstimateLikeField(field)) {
        return FormatEstimateSeconds(raw);
    }
    return raw;
}

/// Day-window keep rule: leading-10-char compare; short/missing timestamps kept
/// unconditionally; an empty bound is open on that side.
bool TimestampInWindow(const std::string& ts, const std::string& dayFrom, const std::string& dayTo) {
    if (ts.size() < 10) {
        return true;
    }
    const std::string day = ts.substr(0, 10);
    if (!dayFrom.empty() && day < dayFrom) {
        return false;
    }
    if (!dayTo.empty() && day > dayTo) {
        return false;
    }
    return true;
}

} // namespace

std::string BuildActivityDiscoveryJql(const std::string& accountId, const std::string& dayFrom,
                                      const std::string& dayTo, const std::string& projectScope) {
    const std::string user = tracker_jql::QuoteLiteral(accountId);
    std::string jql = "(assignee WAS \"" + user + "\" OR reporter WAS \"" + user + "\")";
    if (!dayFrom.empty()) {
        jql += " AND updated >= \"" + tracker_jql::QuoteLiteral(dayFrom) + "\"";
    }
    if (!dayTo.empty()) {
        jql += " AND updated <= \"" + tracker_jql::QuoteLiteral(dayTo) + "\"";
    }
    if (!projectScope.empty()) {
        jql += " AND project = \"" + tracker_jql::QuoteLiteral(projectScope) + "\"";
    }
    jql += " ORDER BY updated DESC";
    return jql;
}

std::string FormatEstimateSeconds(const std::string& rawSeconds) {
    if (rawSeconds.empty()) {
        return rawSeconds;
    }
    for (char c : rawSeconds) {
        if (!IsAsciiDigitChar(c)) {
            return rawSeconds; // parse-failure passthrough
        }
    }
    long long seconds = 0;
    try {
        seconds = std::stoll(rawSeconds);
    } catch (const std::exception&) {
        return rawSeconds; // overflow passthrough
    }
    const long long kWorkdaySec = 8LL * 3600LL;
    const long long days = seconds / kWorkdaySec;
    const long long hours = (seconds % kWorkdaySec) / 3600LL;
    const long long minutes = (seconds % 3600LL) / 60LL;
    std::string out;
    if (days > 0) {
        out += std::to_string(days) + "d";
    }
    if (hours > 0) {
        out += (out.empty() ? "" : " ") + std::to_string(hours) + "h";
    }
    if (minutes > 0) {
        out += (out.empty() ? "" : " ") + std::to_string(minutes) + "m";
    }
    if (out.empty()) {
        out = "0m";
    }
    return out;
}

std::vector<TrackerActivityEntry> EntriesFromIssueJson(const nlohmann::json& issue, const std::string& accountId,
                                                       const std::string& dayFrom, const std::string& dayTo,
                                                       const std::string& browseBase) {
    std::vector<TrackerActivityEntry> entries;
    if (!issue.is_object()) {
        return entries;
    }
    const std::string key = JsonValueAsString(issue, "key");
    std::string summary;
    if (issue.contains("fields") && issue["fields"].is_object()) {
        summary = JsonValueAsString(issue["fields"], "summary");
    }
    if (!issue.contains("changelog") || !issue["changelog"].is_object()) {
        return entries;
    }
    const auto histories = issue["changelog"].value("histories", nlohmann::json::array());
    if (!histories.is_array()) {
        return entries;
    }
    const std::string issueUrl = browseBase.empty() || key.empty() ? std::string() : browseBase + "/browse/" + key;
    for (const auto& history : histories) {
        if (!history.is_object()) {
            continue;
        }
        std::string authorAccountId;
        if (history.contains("author") && history["author"].is_object()) {
            authorAccountId = JsonValueAsString(history["author"], "accountId");
        }
        if (authorAccountId != accountId) {
            continue;
        }
        const std::string created = JsonValueAsString(history, "created");
        if (!TimestampInWindow(created, dayFrom, dayTo)) {
            continue;
        }
        const auto items = history.value("items", nlohmann::json::array());
        if (!items.is_array()) {
            continue;
        }
        for (const auto& item : items) {
            if (!item.is_object()) {
                continue;
            }
            const std::string field = JsonValueAsString(item, "field");
            const std::string from = DisplaySide(field, JsonValueAsString(item, "from"),
                                                 JsonValueAsString(item, "fromString"));
            const std::string to =
                DisplaySide(field, JsonValueAsString(item, "to"), JsonValueAsString(item, "toString"));
            TrackerActivityEntry entry;
            entry.Timestamp = created;
            entry.IssueKey = key;
            entry.IssueUrl = issueUrl;
            entry.Summary = summary;
            entry.ActionLabel = field;
            entry.Details = (from.empty() ? std::string("(none)") : from) + " -> " +
                            (to.empty() ? std::string("(none)") : to);
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

} // namespace JiraActivityFeed

Result<std::vector<TrackerActivityEntry>, TrackerError>
JiraClient::FetchUserActivity(const TrackerConfig& cfg, const std::string& accountId, const std::string& dayFrom,
                              const std::string& dayTo, const std::string& projectScope,
                              TrackerActivityProgress& progress) {
    using FeedResult = Result<std::vector<TrackerActivityEntry>, TrackerError>;
    std::vector<TrackerActivityEntry> outEntries;
    std::string outError;
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return FeedResult::Err(TrackerErrorAuth(outError));
    }
    if (accountId.empty()) {
        return FeedResult::Ok(std::move(outEntries));
    }
    activityCancel_.store(false);

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);
    const std::string jql = JiraActivityFeed::BuildActivityDiscoveryJql(accountId, dayFrom, dayTo, projectScope);
    const std::string baseSearchUrl =
        base + "/rest/api/3/search/jql?jql=" + UrlEncode(jql) + "&maxResults=100&fields=summary&expand=changelog";

    std::string nextPageToken;
    const int kMaxPages = 20; // activity window — bounded scan, not a full sync
    for (int page = 1; page <= kMaxPages; ++page) {
        if (activityCancel_.load()) {
            break; // window closed — return what we have
        }
        std::string pageUrl = baseSearchUrl;
        if (!nextPageToken.empty()) {
            pageUrl += "&nextPageToken=" + UrlEncode(nextPageToken);
        }
        auto resp = TrackerGetLogged("JiraClient", pageUrl, headers);
        if (resp.status_code != 200) {
            outError = "activity search failed: HTTP " + std::to_string(resp.status_code);
            LOG_ERROR("JiraClient: %s account=%s", outError.c_str(), TruncateForLog(accountId, 40).c_str());
            if (resp.status_code >= 200 && resp.status_code < 300) {
                return FeedResult::Err(TrackerErrorUnknown(outError, resp.status_code));
            }
            return FeedResult::Err(TrackerErrorFromHttpStatus(resp.status_code, outError));
        }
        try {
            // Bounded parse — the try can't catch a depth-bomb's destructor-time SIGSEGV
            // (audit: unbounded-recursion-DoS). Discarded on failure → value() below hits the catch.
            auto j = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
            const auto issues = j.value("issues", nlohmann::json::array());
            if (issues.is_array()) {
                progress.Total.fetch_add(static_cast<int>(issues.size()));
                for (const auto& issue : issues) {
                    if (activityCancel_.load()) {
                        break;
                    }
                    std::vector<TrackerActivityEntry> entries =
                        JiraActivityFeed::EntriesFromIssueJson(issue, accountId, dayFrom, dayTo, base);
                    for (auto& entry : entries) {
                        outEntries.push_back(std::move(entry));
                    }
                    progress.Current.fetch_add(1);
                }
            }
            nextPageToken = j.value("nextPageToken", std::string());
        } catch (const std::exception& ex) {
            outError = std::string("activity search parse error: ") + ex.what();
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return FeedResult::Err(TrackerErrorParse(outError));
        }
        if (nextPageToken.empty()) {
            break;
        }
    }

    std::stable_sort(outEntries.begin(), outEntries.end(),
                     [](const TrackerActivityEntry& a, const TrackerActivityEntry& b) {
                         return a.Timestamp > b.Timestamp; // newest first
                     });
    return FeedResult::Ok(std::move(outEntries));
}

void JiraClient::ClearUserActivity() { activityCancel_.store(true); }
