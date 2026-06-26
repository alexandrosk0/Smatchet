#include "GitHubActivityFeed.h"

#include "GitHubClient.h"
#include "GitHubIssueSearch.h"
#include "Logger.h"
#include "StringUtil.h"
#include "TrackerHttpUtils.h"
#include "Json/BoundedJsonParse.h"

#include <algorithm>
#include <string>
#include <vector>

// The pure mapping half (EntriesFromEventsJson / PageEndsBeforeWindow) lives in
// GitHubActivityFeedPure.cpp so the doctest rig links it without this TU's cpr +
// GitHubClient dependencies.

namespace {

/// Resolve the owner/repo pair for an activity scan: an `owner/repo`-shaped
/// projectScope wins, else the configured tracker repo.
void ResolveOwnerRepo(const TrackerConfig& cfg, const std::string& projectScope, std::string& outOwner,
                      std::string& outRepo) {
    outOwner = cfg.GitHubOwner;
    outRepo = cfg.GitHubRepo;
    const size_t slash = projectScope.find('/');
    if (slash != std::string::npos && slash > 0 && slash + 1 < projectScope.size()) {
        outOwner = projectScope.substr(0, slash);
        outRepo = projectScope.substr(slash + 1);
    }
}

} // namespace

Result<std::vector<TrackerActivityEntry>, TrackerError>
GitHubClient::FetchUserActivity(const TrackerConfig& cfg, const std::string& accountId, const std::string& dayFrom,
                                const std::string& dayTo, const std::string& projectScope,
                                TrackerActivityProgress& progress) {
    using FeedResult = Result<std::vector<TrackerActivityEntry>, TrackerError>;
    std::vector<TrackerActivityEntry> outEntries;
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg);
    if (auth.Pat.empty()) {
        return FeedResult::Err(TrackerErrorAuth("GitHub PAT is not configured."));
    }
    if (accountId.empty()) {
        return FeedResult::Ok(std::move(outEntries));
    }
    std::string owner;
    std::string repo;
    ResolveOwnerRepo(cfg, projectScope, owner, repo);
    if (owner.empty() || repo.empty()) {
        return FeedResult::Err(TrackerErrorInvalidRequest(
            "GitHub activity requires an owner/repo scope (set Owner + Repo in Preferences)."));
    }
    activityCancel_.store(false);

    const cpr::Header headers = smatchet::github::BuildGitHubHeaders(auth.Pat);
    const std::string eventsBase = auth.BaseUrl + "/repos/" + owner + "/" + repo + "/issues/events";

    std::string outError;
    const int kMaxPages = 10; // 1000 events — bounded scan, not a full sync
    for (int page = 1; page <= kMaxPages; ++page) {
        if (activityCancel_.load()) {
            break; // window closed — return what we have
        }
        const std::string pageUrl = eventsBase + "?per_page=100&page=" + std::to_string(page);
        auto resp = TrackerGetLogged("GitHubClient", pageUrl, headers);
        if (resp.status_code != 200) {
            outError = "issue-events fetch failed: HTTP " + std::to_string(resp.status_code);
            LOG_ERROR("GitHubClient: %s account=%s", outError.c_str(), TruncateForLog(accountId, 40).c_str());
            if (resp.status_code >= 200 && resp.status_code < 300) {
                return FeedResult::Err(TrackerErrorUnknown(outError, resp.status_code));
            }
            return FeedResult::Err(TrackerErrorFromHttpStatus(resp.status_code, outError));
        }
        std::string parseErr;
        nlohmann::json events = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            outError = std::string("issue-events parse error: ") + parseErr;
            LOG_ERROR("GitHubClient: %s", outError.c_str());
            return FeedResult::Err(TrackerErrorParse(outError));
        }
        if (!events.is_array()) {
            outError = "issue-events: expected array.";
            LOG_ERROR("GitHubClient: %s body=%s", outError.c_str(), RedactHttpBodyForLog(resp.text).c_str());
            return FeedResult::Err(TrackerErrorParse(outError));
        }
        progress.Total.fetch_add(static_cast<int>(events.size()));
        std::vector<TrackerActivityEntry> entries =
            GitHubActivityFeed::EntriesFromEventsJson(events, accountId, dayFrom, dayTo, owner, repo);
        for (auto& entry : entries) {
            outEntries.push_back(std::move(entry));
        }
        progress.Current.fetch_add(static_cast<int>(events.size()));
        // /issues/events is newest-first — once a page's tail precedes the window,
        // no later page can re-enter it.
        if (GitHubActivityFeed::PageEndsBeforeWindow(events, dayFrom) || events.size() < 100) {
            break;
        }
    }

    std::stable_sort(outEntries.begin(), outEntries.end(),
                     [](const TrackerActivityEntry& a, const TrackerActivityEntry& b) {
                         return a.Timestamp > b.Timestamp; // newest first
                     });
    return FeedResult::Ok(std::move(outEntries));
}

Result<std::vector<std::string>, TrackerError> GitHubClient::FetchUserGroupNames(const TrackerConfig& cfg,
                                                                                 const std::string& accountId) {
    using GroupsResult = Result<std::vector<std::string>, TrackerError>;
    std::vector<std::string> outGroupNames;
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg);
    if (auth.Pat.empty()) {
        return GroupsResult::Err(TrackerErrorAuth("GitHub PAT is not configured."));
    }
    const std::string org = cfg.GitHubOwner;
    if (org.empty() || accountId.empty()) {
        return GroupsResult::Ok(std::move(outGroupNames));
    }

    const cpr::Header headers = smatchet::github::BuildGitHubHeaders(auth.Pat);
    const std::string teamsUrl = auth.BaseUrl + "/orgs/" + org + "/teams?per_page=100";
    auto resp = TrackerGetLogged("GitHubClient", teamsUrl, headers);
    if (resp.status_code != 200) {
        // Token-scope-gated degrade (plan user-info-window.md item 18): a PAT without
        // read:org gets 403 (404 when the owner is a user, not an org) — groups are
        // simply unavailable, not an error state for the window.
        LOG_WARN("GitHubClient: org teams unavailable: HTTP %s org=%s (PAT may lack read:org)",
                 std::to_string(resp.status_code).c_str(), TruncateForLog(org, 60).c_str());
        return GroupsResult::Ok(std::move(outGroupNames));
    }

    try {
        std::string parseErr;
        const auto teams = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("GitHubClient: org teams parse error: %s", parseErr.c_str());
            return GroupsResult::Ok(std::move(outGroupNames));
        }
        if (!teams.is_array()) {
            LOG_WARN("GitHubClient: org teams: expected array, body=%s", RedactHttpBodyForLog(resp.text).c_str());
            return GroupsResult::Ok(std::move(outGroupNames));
        }
        const int kMaxTeamChecks = 50; // one membership probe per team — sanity bound
        int checked = 0;
        for (const auto& team : teams) {
            if (checked >= kMaxTeamChecks) {
                break;
            }
            if (!team.is_object()) {
                continue; // skip malformed element — later teams may still be valid
            }
            ++checked;
            const std::string slug =
                team.contains("slug") && team["slug"].is_string() ? team["slug"].get<std::string>() : std::string();
            const std::string name =
                team.contains("name") && team["name"].is_string() ? team["name"].get<std::string>() : slug;
            if (slug.empty()) {
                continue;
            }
            const std::string membershipUrl =
                auth.BaseUrl + "/orgs/" + org + "/teams/" + slug + "/memberships/" + UrlEncode(accountId);
            auto membership = TrackerGetLogged("GitHubClient", membershipUrl, headers);
            if (membership.status_code == 200) { // 404 = not a member — skip silently
                outGroupNames.push_back(name);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("GitHubClient: org teams parse error: %s", ex.what());
        return GroupsResult::Ok(std::move(outGroupNames));
    }
    return GroupsResult::Ok(std::move(outGroupNames));
}

Result<std::vector<TrackerUser>, TrackerError> GitHubClient::FetchGroupMembers(const TrackerConfig& cfg,
                                                                               const std::string& groupName) {
    using MembersResult = Result<std::vector<TrackerUser>, TrackerError>;
    std::vector<TrackerUser> outMembers;
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg);
    if (auth.Pat.empty()) {
        return MembersResult::Err(TrackerErrorAuth("GitHub PAT is not configured."));
    }
    const std::string org = cfg.GitHubOwner;
    if (org.empty() || groupName.empty()) {
        return MembersResult::Ok(std::move(outMembers));
    }

    const cpr::Header headers = smatchet::github::BuildGitHubHeaders(auth.Pat);

    // The members endpoint is slug-addressed; the roster cache hands back the display
    // name FetchUserGroupNames returned — resolve it to a slug via the team list.
    std::string slug;
    auto teamsResp = TrackerGetLogged("GitHubClient", auth.BaseUrl + "/orgs/" + org + "/teams?per_page=100", headers);
    if (teamsResp.status_code != 200) {
        LOG_WARN("GitHubClient: org teams unavailable: HTTP %s org=%s (PAT may lack read:org)",
                 std::to_string(teamsResp.status_code).c_str(), TruncateForLog(org, 60).c_str());
        return MembersResult::Ok(std::move(outMembers));
    }
    try {
        std::string parseErr;
        const auto teams = smatchet::json_safe::ParseBounded(teamsResp.text, parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("GitHubClient: org teams parse error: %s", parseErr.c_str());
            return MembersResult::Ok(std::move(outMembers));
        }
        const std::string wanted = ToLowerAsciiCopy(groupName);
        if (teams.is_array()) {
            for (const auto& team : teams) {
                if (!team.is_object()) {
                    continue;
                }
                const std::string name =
                    team.contains("name") && team["name"].is_string() ? team["name"].get<std::string>() : std::string();
                const std::string teamSlug =
                    team.contains("slug") && team["slug"].is_string() ? team["slug"].get<std::string>() : std::string();
                if (ToLowerAsciiCopy(name) == wanted || ToLowerAsciiCopy(teamSlug) == wanted) {
                    slug = teamSlug;
                    break;
                }
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("GitHubClient: org teams parse error: %s", ex.what());
        return MembersResult::Ok(std::move(outMembers));
    }
    if (slug.empty()) {
        return MembersResult::Ok(std::move(outMembers));
    }

    const std::string membersUrl = auth.BaseUrl + "/orgs/" + org + "/teams/" + slug + "/members?per_page=100";
    auto resp = TrackerGetLogged("GitHubClient", membersUrl, headers);
    if (resp.status_code != 200) {
        LOG_WARN("GitHubClient: team members unavailable: HTTP %s team=%s", std::to_string(resp.status_code).c_str(),
                 TruncateForLog(slug, 60).c_str());
        return MembersResult::Ok(std::move(outMembers));
    }
    try {
        std::string parseErr;
        const auto members = smatchet::json_safe::ParseBounded(resp.text, parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("GitHubClient: team members parse error: %s", parseErr.c_str());
            return MembersResult::Ok(std::move(outMembers));
        }
        if (members.is_array()) {
            for (const auto& member : members) {
                if (!member.is_object() || !member.contains("login") || !member["login"].is_string()) {
                    continue;
                }
                TrackerUser u;
                u.AccountId = member["login"].get<std::string>();
                u.DisplayName = u.AccountId; // GitHub member list carries logins only
                outMembers.push_back(std::move(u));
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("GitHubClient: team members parse error: %s", ex.what());
        return MembersResult::Ok(std::move(outMembers));
    }
    return MembersResult::Ok(std::move(outMembers));
}

void GitHubClient::ClearUserActivity() { activityCancel_.store(true); }
