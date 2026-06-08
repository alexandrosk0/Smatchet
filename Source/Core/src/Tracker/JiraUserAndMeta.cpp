#include "JiraClient.h"

#include "JiraEditMetaPure.h"
#include "TrackerFieldValueParser.h"
#include "TrackerHttpUtils.h"
#include "JsonParseUtil.h"
#include "Logger.h"
#include "StringUtil.h"

#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

void ParseTrackerUserObject(const nlohmann::json& user, TrackerUser& out) {
    out.AccountId = user.value("accountId", std::string());
    out.DisplayName = user.value("displayName", std::string());
    out.EmailAddress = user.value("emailAddress", std::string());
    out.AccountType = user.value("accountType", std::string());
    out.Active = user.value("active", true);
}

} // namespace

bool JiraClient::FetchUsers(const TrackerConfig& cfg, std::vector<TrackerUser>& outUsers, std::string& outError) {
    outUsers.clear();
    outError.clear();

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string usersUrl = base + "/rest/api/3/users/search?maxResults=1000";
    auto usersResponse = TrackerGetLogged("JiraClient", usersUrl, headers);
    if (usersResponse.status_code != 200) {
        outError = "Failed to fetch users: HTTP " + std::to_string(usersResponse.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        auto usersJson = nlohmann::json::parse(usersResponse.text);
        if (!usersJson.is_array()) {
            outError = "Invalid users response format.";
            LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), TruncateForLog(usersResponse.text, 300).c_str());
            return false;
        }

        std::set<std::string> seenAccountIds;
        std::set<std::string> seenDisplayNames;
        for (const auto& user : usersJson) {
            TrackerUser TrackerUser;
            TrackerUser.AccountId = user.value("accountId", std::string());
            TrackerUser.DisplayName = user.value("displayName", std::string());
            TrackerUser.EmailAddress = user.value("emailAddress", std::string());
            TrackerUser.AccountType = user.value("accountType", std::string());
            TrackerUser.Active = user.value("active", true);

            if (TrackerUser.DisplayName.empty() || TrackerUser.AccountId.empty() || !TrackerUser.Active ||
                !seenAccountIds.insert(TrackerUser.AccountId).second) {
                continue;
            }

            if (!seenDisplayNames.insert(TrackerUser.DisplayName).second) {
                const std::string suffix =
                    TrackerUser.AccountId.substr(0, std::min<size_t>(4, TrackerUser.AccountId.size()));
                TrackerUser.DisplayName += "_" + suffix;
            }

            outUsers.push_back(std::move(TrackerUser));
        }

        SortTrackerUsersForDisplay(outUsers);
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse users response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    return true;
}

bool JiraClient::FetchIssueWatchers(const TrackerConfig& cfg, const std::string& issueKey,
                                    std::vector<TrackerUser>& outWatchers, std::string& outError) {
    outWatchers.clear();
    outError.clear();

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/watchers";
    auto response = TrackerGetLogged("JiraClient", url, headers);
    if (response.status_code != 200) {
        outError = "Failed to fetch watchers: HTTP " + std::to_string(response.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        const auto j = nlohmann::json::parse(response.text);
        if (!j.is_object()) {
            outError = "Invalid watchers response format.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKey.c_str(),
                      TruncateForLog(response.text, 300).c_str());
            return false;
        }
        const auto watchers = j.value("watchers", nlohmann::json::array());
        if (!watchers.is_array()) {
            outError = "Invalid watchers array in response.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKey.c_str(),
                      TruncateForLog(response.text, 300).c_str());
            return false;
        }
        AppendTrackerUsersFromJsonArray(watchers, outWatchers);
        SortTrackerUsersForDisplay(outWatchers);
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse watchers response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    return true;
}

bool JiraClient::AddIssueWatcher(const TrackerConfig& cfg, const std::string& issueKey, std::string& outError) {
    outError.clear();
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        LOG_ERROR("JiraClient::AddIssueWatcher %s", outError.c_str());
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);

    const std::string myselfUrl = base + "/rest/api/3/myself";
    auto myselfResp = TrackerGetLogged("JiraClient", myselfUrl, headers);
    if (myselfResp.status_code != 200) {
        outError = "Failed to fetch current user: HTTP " + std::to_string(myselfResp.status_code);
        LOG_ERROR("JiraClient::AddIssueWatcher myself failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
        return false;
    }
    std::string accountId;
    try {
        const auto me = nlohmann::json::parse(myselfResp.text);
        accountId = me.value("accountId", std::string());
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse myself response: ") + ex.what();
        LOG_ERROR("JiraClient::AddIssueWatcher %s", outError.c_str());
        return false;
    }
    if (accountId.empty()) {
        outError = "Could not determine current user accountId.";
        LOG_ERROR("JiraClient::AddIssueWatcher %s issue=%s", outError.c_str(), issueKey.c_str());
        return false;
    }

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/watchers";
    const nlohmann::json bodyJson = accountId;
    const std::string bodyStr = bodyJson.dump();
    auto response = TrackerPostLogged("JiraClient", url, headers, bodyStr);
    if (response.status_code != 204 && response.status_code != 200) {
        outError = "Failed to add watcher: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " - " + response.text.substr(0, 200);
        }
        LOG_ERROR("JiraClient::AddIssueWatcher failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
        return false;
    }
    return true;
}

Result<std::unordered_map<std::string, bool>, TrackerError>
JiraClient::FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& issueKeyOrId) {
    using EditMetaResult = Result<std::unordered_map<std::string, bool>, TrackerError>;
    std::unordered_map<std::string, bool> outFieldIdCanEdit;
    std::string outError;

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return EditMetaResult::Err(TrackerErrorInvalidRequest(std::move(outError)));
    }
    if (issueKeyOrId.empty()) {
        return EditMetaResult::Err(TrackerErrorInvalidRequest("Issue key or id is empty."));
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);
    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKeyOrId) + "/editmeta";
    auto response = TrackerGetLogged("JiraClient", url, headers);
    if (response.status_code != 200) {
        outError = "Failed to fetch issue editmeta: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " — ";
            outError += TruncateForLog(response.text, 800);
        }
        LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueKeyOrId.c_str());
        // A 2xx-non-200 (201/202/204) reaches this failure branch; for a 2xx, TrackerErrorFromHttpStatus
        // yields an Ok sentinel (Kind==None, detail discarded). Carry the verbatim detail under an
        // explicit non-OK kind so the user-visible .Detail never vanishes. Genuine non-2xx classifies normally.
        if (response.status_code >= 200 && response.status_code < 300) {
            return EditMetaResult::Err(TrackerErrorUnknown(std::move(outError), response.status_code));
        }
        return EditMetaResult::Err(TrackerErrorFromHttpStatus(response.status_code, std::move(outError)));
    }

    try {
        const auto root = nlohmann::json::parse(response.text);
        if (!root.is_object() || !root.contains("fields") || !root["fields"].is_object()) {
            outError = "Invalid editmeta response: missing fields object.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKeyOrId.c_str(),
                      TruncateForLog(response.text, 400).c_str());
            return EditMetaResult::Err(TrackerErrorParse(std::move(outError)));
        }
        const auto& fields = root["fields"];
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            const std::string fieldId = ToLowerAsciiCopy(it.key());
            outFieldIdCanEdit[fieldId] = smatchet::jira::JiraEditMetaFieldCanEdit(fieldId, it.value());
        }
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse editmeta response: ") + ex.what();
        LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueKeyOrId.c_str());
        return EditMetaResult::Err(TrackerErrorParse(std::move(outError)));
    }

    return EditMetaResult::Ok(std::move(outFieldIdCanEdit));
}

bool JiraClient::FetchIssueVotes(const TrackerConfig& cfg, const std::string& issueKey,
                                 std::vector<TrackerUser>& outVoters, std::string& outError, int* outVoteCount,
                                 bool* outHasVoted, bool* outVotersArrayInResponse) {
    outVoters.clear();
    outError.clear();
    if (outVoteCount) {
        *outVoteCount = 0;
    }
    if (outHasVoted) {
        *outHasVoted = false;
    }
    if (outVotersArrayInResponse) {
        *outVotersArrayInResponse = false;
    }

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/votes";
    auto response = TrackerGetLogged("JiraClient", url, headers);
    if (response.status_code != 200) {
        outError = "Failed to fetch votes: HTTP " + std::to_string(response.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        const auto j = nlohmann::json::parse(response.text);
        if (!j.is_object()) {
            outError = "Invalid votes response format.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKey.c_str(),
                      TruncateForLog(response.text, 300).c_str());
            return false;
        }

        if (outVoteCount && j.contains("votes")) {
            *outVoteCount = ParseJsonIntLoose(j["votes"], 0);
            if (*outVoteCount < 0) {
                *outVoteCount = 0;
            }
        }

        if (outHasVoted && j.contains("hasVoted")) {
            const auto& hv = j["hasVoted"];
            if (hv.is_boolean()) {
                *outHasVoted = hv.get<bool>();
            } else if (hv.is_number_integer()) {
                *outHasVoted = (hv.get<long long>() != 0);
            }
        }

        if (j.contains("voters")) {
            if (outVotersArrayInResponse) {
                *outVotersArrayInResponse = j["voters"].is_array();
            }
            const auto voters = j.value("voters", nlohmann::json::array());
            if (voters.is_array()) {
                AppendTrackerUsersFromJsonArray(voters, outVoters);
                SortTrackerUsersForDisplay(outVoters);
            }
        }
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse votes response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    return true;
}

bool JiraClient::SearchUsersByQuery(const TrackerConfig& cfg, const std::string& query,
                                    std::vector<TrackerUser>& outUsers, std::string& outError) {
    outUsers.clear();
    outError.clear();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Tracker domain or API token.";
        return false;
    }
    if (query.empty()) {
        return true;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url = base + "/rest/api/3/user/search?query=" + UrlEncode(query) + "&maxResults=100";
    auto resp = TrackerGetLogged("JiraClient", url, headers);
    if (resp.status_code != 200) {
        outError = "user/search failed: HTTP " + std::to_string(resp.status_code);
        LOG_ERROR("JiraClient: %s query=%s", outError.c_str(), TruncateForLog(query, 120).c_str());
        return false;
    }

    try {
        auto arr = nlohmann::json::parse(resp.text);
        if (!arr.is_array()) {
            outError = "user/search: expected array.";
            LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), TruncateForLog(resp.text, 300).c_str());
            return false;
        }
        std::unordered_set<std::string> seen;
        for (const auto& node : arr) {
            if (!node.is_object()) {
                continue;
            }
            TrackerUser u;
            ParseTrackerUserObject(node, u);
            if (u.AccountId.empty() || !u.Active || !seen.insert(u.AccountId).second) {
                continue;
            }
            outUsers.push_back(std::move(u));
        }
    } catch (const std::exception& ex) {
        outError = std::string("user/search parse error: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }
    return true;
}

bool JiraClient::FetchUserGroupNames(const TrackerConfig& cfg, const std::string& accountId,
                                     std::vector<std::string>& outGroupNames, std::string& outError) {
    outGroupNames.clear();
    outError.clear();
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return false;
    }
    if (accountId.empty()) {
        return true;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url =
        base + "/rest/api/3/user?accountId=" + UrlEncode(accountId) + "&expand=groups,applicationRoles";
    auto resp = TrackerGetLogged("JiraClient", url, headers);
    if (resp.status_code != 200) {
        outError = "user lookup failed: HTTP " + std::to_string(resp.status_code);
        LOG_ERROR("JiraClient: %s accountId=%s", outError.c_str(), TruncateForLog(accountId, 40).c_str());
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp.text);
        if (j.contains("groups") && j["groups"].is_object()) {
            const auto& g = j["groups"];
            if (g.contains("items") && g["items"].is_array()) {
                for (const auto& item : g["items"]) {
                    if (item.is_object() && item.contains("name") && item["name"].is_string()) {
                        outGroupNames.push_back(item["name"].get<std::string>());
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        outError = std::string("user parse error: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }
    return true;
}
