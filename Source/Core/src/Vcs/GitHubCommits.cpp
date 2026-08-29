#include "Vcs/GitHubCommits.h"

#include "GitHubRestHeaders.h"

#include "Logger.h"
#include "StringUtil.h"
#include "Tracker/GitHubClientHelpers.h"
#include "Tracker/TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

namespace Vcs {

namespace {

// GitHub REST headers for the VCS commit feed (shared recipe, commit-feed User-Agent).
cpr::Header GitHubCommitFeedHeaders(const std::string& pat) {
    return smatchet::github::GitHubRestHeaders(pat, "Smatchet-VcsCommitFeed");
}

void AppendError(std::string& outError, const std::string& msg) {
    if (!outError.empty()) {
        outError += "; ";
    }
    outError += msg;
}

} // namespace

bool GitHubCommitsForUser(const TrackerConfig& cfg, const std::string& authorEmailOrLogin, int maxN,
                          std::vector<VcsSubmission>& outCommits, std::string& outError) {
    outCommits.clear();
    outError.clear();
    if (cfg.GitHubPat.empty()) {
        outError = "GitHub PAT not configured";
        return false;
    }
    std::vector<std::string> repos = SplitAndTrim(cfg.GitCommitRepos, ',');
    if (repos.empty()) {
        // No explicit git_commit_repos: reuse the GitHub tracker's own owner/repo
        // so the commit feed works out of the box once the tracker is connected.
        const std::string owner = TrimCopy(cfg.GitHubOwner);
        const std::string repo = TrimCopy(cfg.GitHubRepo);
        if (!owner.empty() && !repo.empty()) {
            repos.push_back(owner + "/" + repo);
            LOG_DEBUG("GitHubCommitsForUser: git_commit_repos empty, falling back to tracker repo '%s/%s'",
                      owner.c_str(), repo.c_str());
        }
    }
    if (repos.empty()) {
        outError = "no git repos configured";
        return false;
    }
    if (authorEmailOrLogin.empty()) {
        outError = "empty git author";
        return false;
    }
    const int capped = maxN > 0 ? maxN : 1;
    const smatchet::github::GitHubRequestAuth auth =
        smatchet::github::ResolveGitHubRequestAuth(cfg.GitHubBaseUrl, cfg.GitHubPat, std::string());
    const cpr::Header headers = GitHubCommitFeedHeaders(auth.Pat);
    for (size_t i = 0; i < repos.size(); ++i) {
        const size_t slash = repos[i].find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= repos[i].size()) {
            AppendError(outError, "bad repo entry '" + repos[i] + "' (want owner/repo)");
            LOG_WARN("GitHubCommitsForUser: bad repo entry '%s'", repos[i].c_str());
            continue;
        }
        const std::string owner = repos[i].substr(0, slash);
        const std::string repo = repos[i].substr(slash + 1);
        const std::string url = auth.BaseUrl + smatchet::github::BuildCommitListUrlSuffix(owner, repo, capped) +
                                "&author=" + UrlEncode(authorEmailOrLogin);
        const cpr::Response resp = TrackerGetLogged("GitHubCommits", url, headers);
        if (resp.status_code != 200) {
            AppendError(outError,
                        repos[i] + ": " +
                            smatchet::github::ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text));
            LOG_WARN("GitHubCommitsForUser: %s HTTP %ld", repos[i].c_str(), resp.status_code);
            continue;
        }
        std::vector<VcsSubmission> repoRows = ParseGitHubCommitListJson(resp.text);
        LOG_DEBUG("GitHubCommitsForUser: %s rows=%zu", repos[i].c_str(), repoRows.size());
        outCommits = MergeFeedsNewestFirst(outCommits, repoRows);
    }
    if (outCommits.size() > static_cast<size_t>(capped)) {
        outCommits.resize(static_cast<size_t>(capped));
    }
    return true;
}

} // namespace Vcs
