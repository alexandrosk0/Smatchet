#include "Vcs/GitHubCommits.h"

#include "Logger.h"
#include "StringUtil.h"
#include "Tracker/GitHubClientHelpers.h"
#include "Tracker/TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

namespace Vcs {

namespace {

// GitHub REST headers for the VCS commit feed (Bearer PAT). Mirrors GitHubClient's
// BuildGitHubHeaders, which is declared in the Tracker-private GitHubIssueSearch.h —
// same local-mirror convention as BugReportService's BugReportGitHubHeaders.
cpr::Header GitHubCommitFeedHeaders(const std::string& pat) {
    return cpr::Header{
        {"Authorization", std::string("Bearer ") + pat},
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "Smatchet-VcsCommitFeed"},
    };
}

std::string FirstLineOf(const std::string& s) {
    const size_t nl = s.find('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

void AppendError(std::string& outError, const std::string& msg) {
    if (!outError.empty()) {
        outError += "; ";
    }
    outError += msg;
}

} // namespace

std::vector<VcsSubmission> ParseGitHubCommitListJson(const std::string& body) {
    std::vector<VcsSubmission> rows;
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        return rows;
    }
    rows.reserve(parsed.size());
    for (const auto& c : parsed) {
        if (!c.is_object()) {
            continue;
        }
        try {
            VcsSubmission s;
            s.Source = VcsSource::Git;
            s.Id = c.value("sha", std::string());
            if (s.Id.empty()) {
                continue;
            }
            s.Url = c.value("html_url", std::string());
            if (c.contains("commit") && c["commit"].is_object()) {
                const nlohmann::json& commit = c["commit"];
                s.FirstLine = FirstLineOf(commit.value("message", std::string()));
                if (commit.contains("author") && commit["author"].is_object()) {
                    s.Timestamp = commit["author"].value("date", std::string());
                    s.Author = commit["author"].value("name", std::string());
                }
            }
            if (s.Author.empty() && c.contains("author") && c["author"].is_object()) {
                s.Author = c["author"].value("login", std::string());
            }
            rows.push_back(std::move(s));
        } catch (const std::exception& ex) {
            // Malformed commit entry (wrong-typed field) — skip the row, keep the rest (Pillar 3).
            LOG_DEBUG("ParseGitHubCommitListJson: skipped malformed commit entry: %s", ex.what());
        }
    }
    return rows;
}

bool GitHubCommitsForUser(const TrackerConfig& cfg, const std::string& authorEmailOrLogin, int maxN,
                          std::vector<VcsSubmission>& outCommits, std::string& outError) {
    outCommits.clear();
    outError.clear();
    if (cfg.GitHubPat.empty()) {
        outError = "GitHub PAT not configured";
        return false;
    }
    const std::vector<std::string> repos = SplitAndTrim(cfg.GitCommitRepos, ',');
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
