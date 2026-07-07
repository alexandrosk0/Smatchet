#include "JiraClient.h"

#include "JiraEditMetaPure.h"
#include "TrackerFieldValueParser.h"
#include "TrackerHttpUtils.h"
#include "JsonParseUtil.h"
#include "Json/BoundedJsonParse.h"
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
        std::string parseErr;
        auto usersJson = smatchet::json_safe::ParseBounded(usersResponse.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("Failed to parse users response: ") + parseErr;
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return false;
        }
        if (!usersJson.is_array()) {
            outError = "Invalid users response format.";
            LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), RedactHttpBodyForLog(usersResponse.text).c_str());
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

Result<std::vector<TrackerUser>, TrackerError> JiraClient::FetchIssueWatchers(const TrackerConfig& cfg,
                                                                              const std::string& issueKey) {
    using WatchersResult = Result<std::vector<TrackerUser>, TrackerError>;
    std::vector<TrackerUser> outWatchers;
    std::string outError;

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return WatchersResult::Err(TrackerErrorAuth(outError));
    }
    if (issueKey.empty()) {
        return WatchersResult::Err(TrackerErrorInvalidRequest("Issue key is empty."));
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/watchers";
    auto response = TrackerGetLogged("JiraClient", url, headers);
    if (response.status_code != 200) {
        outError = "Failed to fetch watchers: HTTP " + std::to_string(response.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        if (response.status_code >= 200 && response.status_code < 300) {
            return WatchersResult::Err(TrackerErrorUnknown(outError, response.status_code));
        }
        return WatchersResult::Err(TrackerErrorFromHttpStatus(response.status_code, outError));
    }

    try {
        std::string parseErr;
        const auto j = smatchet::json_safe::ParseBounded(response.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("Failed to parse watchers response: ") + parseErr;
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return WatchersResult::Err(TrackerErrorParse(outError));
        }
        if (!j.is_object()) {
            outError = "Invalid watchers response format.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKey.c_str(),
                      RedactHttpBodyForLog(response.text).c_str());
            return WatchersResult::Err(TrackerErrorParse(outError));
        }
        const auto watchers = j.value("watchers", nlohmann::json::array());
        if (!watchers.is_array()) {
            outError = "Invalid watchers array in response.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKey.c_str(),
                      RedactHttpBodyForLog(response.text).c_str());
            return WatchersResult::Err(TrackerErrorParse(outError));
        }
        AppendTrackerUsersFromJsonArray(watchers, outWatchers);
        SortTrackerUsersForDisplay(outWatchers);
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse watchers response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return WatchersResult::Err(TrackerErrorParse(outError));
    }

    return WatchersResult::Ok(std::move(outWatchers));
}

TrackerError JiraClient::AddIssueWatcher(const TrackerConfig& cfg, const std::string& issueKey) {
    std::string outError;
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return TrackerErrorAuth(outError);
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        LOG_ERROR("JiraClient::AddIssueWatcher %s", outError.c_str());
        return TrackerErrorInvalidRequest(outError);
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);

    const std::string myselfUrl = base + "/rest/api/3/myself";
    auto myselfResp = TrackerGetLogged("JiraClient", myselfUrl, headers);
    if (myselfResp.status_code != 200) {
        outError = "Failed to fetch current user: HTTP " + std::to_string(myselfResp.status_code);
        LOG_ERROR("JiraClient::AddIssueWatcher myself failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
        if (myselfResp.status_code >= 200 && myselfResp.status_code < 300) {
            return TrackerErrorUnknown(outError, myselfResp.status_code);
        }
        return TrackerErrorFromHttpStatus(myselfResp.status_code, outError);
    }
    std::string accountId;
    try {
        std::string parseErr;
        const auto me = smatchet::json_safe::ParseBounded(myselfResp.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("Failed to parse myself response: ") + parseErr;
            LOG_ERROR("JiraClient::AddIssueWatcher %s", outError.c_str());
            return TrackerErrorParse(outError);
        }
        accountId = me.value("accountId", std::string());
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse myself response: ") + ex.what();
        LOG_ERROR("JiraClient::AddIssueWatcher %s", outError.c_str());
        return TrackerErrorParse(outError);
    }
    if (accountId.empty()) {
        outError = "Could not determine current user accountId.";
        LOG_ERROR("JiraClient::AddIssueWatcher %s issue=%s", outError.c_str(), issueKey.c_str());
        return TrackerErrorParse(outError);
    }

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/watchers";
    const nlohmann::json bodyJson = accountId;
    const std::string bodyStr = bodyJson.dump();
    auto response = TrackerPostLogged("JiraClient", url, headers, bodyStr);
    if (response.status_code != 204 && response.status_code != 200) {
        outError = "Failed to add watcher: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            // Redact tokens before splicing the raw body into the error / log — a
            // 401/403 body can reflect the request's Authorization / x-api-key.
            outError += " - " + RedactHttpBodyForLog(response.text).substr(0, 200);
        }
        LOG_ERROR("JiraClient::AddIssueWatcher failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
        if (response.status_code >= 200 && response.status_code < 300) {
            return TrackerErrorUnknown(outError, response.status_code);
        }
        return TrackerErrorFromHttpStatus(response.status_code, outError);
    }
    return TrackerError::Ok();
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
        std::string parseErr;
        const auto root = smatchet::json_safe::ParseBounded(response.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("Failed to parse editmeta response: ") + parseErr;
            LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueKeyOrId.c_str());
            return EditMetaResult::Err(TrackerErrorParse(std::move(outError)));
        }
        if (!root.is_object() || !root.contains("fields") || !root["fields"].is_object()) {
            outError = "Invalid editmeta response: missing fields object.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKeyOrId.c_str(),
                      RedactHttpBodyForLog(response.text).c_str());
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

Result<TrackerIssueVotes, TrackerError> JiraClient::FetchIssueVotes(const TrackerConfig& cfg,
                                                                    const std::string& issueKey) {
    using VotesResult = Result<TrackerIssueVotes, TrackerError>;
    TrackerIssueVotes votes;
    std::string outError;

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return VotesResult::Err(TrackerErrorAuth(outError));
    }
    if (issueKey.empty()) {
        return VotesResult::Err(TrackerErrorInvalidRequest("Issue key is empty."));
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/votes";
    auto response = TrackerGetLogged("JiraClient", url, headers);
    if (response.status_code != 200) {
        outError = "Failed to fetch votes: HTTP " + std::to_string(response.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        if (response.status_code >= 200 && response.status_code < 300) {
            return VotesResult::Err(TrackerErrorUnknown(outError, response.status_code));
        }
        return VotesResult::Err(TrackerErrorFromHttpStatus(response.status_code, outError));
    }

    try {
        std::string parseErr;
        const auto j = smatchet::json_safe::ParseBounded(response.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("Failed to parse votes response: ") + parseErr;
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return VotesResult::Err(TrackerErrorParse(outError));
        }
        if (!j.is_object()) {
            outError = "Invalid votes response format.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueKey.c_str(),
                      RedactHttpBodyForLog(response.text).c_str());
            return VotesResult::Err(TrackerErrorParse(outError));
        }

        if (j.contains("votes")) {
            votes.VoteCount = ParseJsonIntLoose(j["votes"], 0);
            if (votes.VoteCount < 0) {
                votes.VoteCount = 0;
            }
        }

        if (j.contains("hasVoted")) {
            const auto& hv = j["hasVoted"];
            if (hv.is_boolean()) {
                votes.HasVoted = hv.get<bool>();
            } else if (hv.is_number_integer()) {
                votes.HasVoted = (hv.get<long long>() != 0);
            }
        }

        if (j.contains("voters")) {
            votes.VotersArrayInResponse = j["voters"].is_array();
            const auto voters = j.value("voters", nlohmann::json::array());
            if (voters.is_array()) {
                AppendTrackerUsersFromJsonArray(voters, votes.Voters);
                SortTrackerUsersForDisplay(votes.Voters);
            }
        }
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse votes response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return VotesResult::Err(TrackerErrorParse(outError));
    }

    return VotesResult::Ok(std::move(votes));
}

Result<std::vector<TrackerUser>, TrackerError> JiraClient::SearchUsersByQuery(const TrackerConfig& cfg,
                                                                              const std::string& query) {
    using UsersResult = Result<std::vector<TrackerUser>, TrackerError>;
    std::vector<TrackerUser> outUsers;
    std::string outError;
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        return UsersResult::Err(TrackerErrorAuth("Missing Tracker domain or API token."));
    }
    if (query.empty()) {
        return UsersResult::Ok(std::move(outUsers));
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url = base + "/rest/api/3/user/search?query=" + UrlEncode(query) + "&maxResults=100";
    // SMATCHET_DEVIATION(rule=duplication; reason=pre-existing boilerplate / include-block clone surfaced by the ParseBounded security sweep touching this file; de-duping independent subsystems is DRY-CRITICAL; owner=security-audit; revisit=2026-09-30)
    auto resp = TrackerGetLogged("JiraClient", url, headers);
    if (resp.status_code != 200) {
        outError = "user/search failed: HTTP " + std::to_string(resp.status_code);
        LOG_ERROR("JiraClient: %s query=%s", outError.c_str(), TruncateForLog(query, 120).c_str());
        if (resp.status_code >= 200 && resp.status_code < 300) {
            return UsersResult::Err(TrackerErrorUnknown(outError, resp.status_code));
        }
        return UsersResult::Err(TrackerErrorFromHttpStatus(resp.status_code, outError));
    }

    try {
        std::string parseErr;
        auto arr = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("user/search parse error: ") + parseErr;
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return UsersResult::Err(TrackerErrorParse(outError));
        }
        if (!arr.is_array()) {
            outError = "user/search: expected array.";
            LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), RedactHttpBodyForLog(resp.text).c_str());
            return UsersResult::Err(TrackerErrorParse(outError));
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
        return UsersResult::Err(TrackerErrorParse(outError));
    }
    return UsersResult::Ok(std::move(outUsers));
}

// ITrackerActivity override (relocated from ITrackerCollaboration — one home per capability).
Result<std::vector<std::string>, TrackerError> JiraClient::FetchUserGroupNames(const TrackerConfig& cfg,
                                                                               const std::string& accountId) {
    using GroupsResult = Result<std::vector<std::string>, TrackerError>;
    std::vector<std::string> outGroupNames;
    std::string outError;
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return GroupsResult::Err(TrackerErrorAuth(outError));
    }
    if (accountId.empty()) {
        return GroupsResult::Ok(std::move(outGroupNames));
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const std::string url =
        base + "/rest/api/3/user?accountId=" + UrlEncode(accountId) + "&expand=groups,applicationRoles";
    // SMATCHET_DEVIATION(rule=duplication; reason=pre-existing boilerplate / include-block clone surfaced by the ParseBounded security sweep touching this file; de-duping independent subsystems is DRY-CRITICAL; owner=security-audit; revisit=2026-09-30)
    auto resp = TrackerGetLogged("JiraClient", url, headers);
    if (resp.status_code != 200) {
        outError = "user lookup failed: HTTP " + std::to_string(resp.status_code);
        LOG_ERROR("JiraClient: %s accountId=%s", outError.c_str(), TruncateForLog(accountId, 40).c_str());
        if (resp.status_code >= 200 && resp.status_code < 300) {
            return GroupsResult::Err(TrackerErrorUnknown(outError, resp.status_code));
        }
        return GroupsResult::Err(TrackerErrorFromHttpStatus(resp.status_code, outError));
    }

    try {
        std::string parseErr;
        auto j = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("user parse error: ") + parseErr;
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return GroupsResult::Err(TrackerErrorParse(outError));
        }
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
        return GroupsResult::Err(TrackerErrorParse(outError));
    }
    return GroupsResult::Ok(std::move(outGroupNames));
}

Result<std::vector<TrackerUser>, TrackerError> JiraClient::FetchGroupMembers(const TrackerConfig& cfg,
                                                                             const std::string& groupName) {
    using MembersResult = Result<std::vector<TrackerUser>, TrackerError>;
    std::vector<TrackerUser> outMembers;
    std::string outError;
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        return MembersResult::Err(TrackerErrorAuth(outError));
    }
    if (groupName.empty()) {
        return MembersResult::Ok(std::move(outMembers));
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    // GET /rest/api/3/group/member — paginated (startAt/maxResults, isLast terminator).
    const int kPageSize = 50;
    const int kMaxPages = 20; // 1000 members — sanity bound for a UI roster
    std::unordered_set<std::string> seen;
    for (int page = 0; page < kMaxPages; ++page) {
        const std::string url = base + "/rest/api/3/group/member?groupname=" + UrlEncode(groupName) +
                                "&startAt=" + std::to_string(page * kPageSize) +
                                "&maxResults=" + std::to_string(kPageSize);
        auto resp = TrackerGetLogged("JiraClient", url, headers);
        if (resp.status_code != 200) {
            outError = "group/member failed: HTTP " + std::to_string(resp.status_code);
            LOG_ERROR("JiraClient: %s group=%s", outError.c_str(), TruncateForLog(groupName, 80).c_str());
            if (resp.status_code >= 200 && resp.status_code < 300) {
                return MembersResult::Err(TrackerErrorUnknown(outError, resp.status_code));
            }
            return MembersResult::Err(TrackerErrorFromHttpStatus(resp.status_code, outError));
        }

        bool isLast = true;
        try {
            std::string parseErr;
            // SMATCHET_DEVIATION(rule=duplication; reason=pre-existing boilerplate / include-block clone surfaced by the ParseBounded security sweep touching this file; de-duping independent subsystems is DRY-CRITICAL; owner=security-audit; revisit=2026-09-30)
            auto j = smatchet::json_safe::ParseBounded(resp.text, parseErr);
            if (!parseErr.empty()) {
                outError = std::string("group/member parse error: ") + parseErr;
                LOG_ERROR("JiraClient: %s", outError.c_str());
                return MembersResult::Err(TrackerErrorParse(outError));
            }
            const auto values = j.value("values", nlohmann::json::array());
            if (values.is_array()) {
                for (const auto& node : values) {
                    if (!node.is_object()) {
                        continue;
                    }
                    TrackerUser u;
                    ParseTrackerUserObject(node, u);
                    if (u.AccountId.empty() || !u.Active || !seen.insert(u.AccountId).second) {
                        continue;
                    }
                    outMembers.push_back(std::move(u));
                }
            }
            isLast = j.value("isLast", true);
        } catch (const std::exception& ex) {
            outError = std::string("group/member parse error: ") + ex.what();
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return MembersResult::Err(TrackerErrorParse(outError));
        }
        if (isLast) {
            break;
        }
    }
    return MembersResult::Ok(std::move(outMembers));
}
