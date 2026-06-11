#include "Vcs/GitHubCommitsParse.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

namespace Vcs {

namespace {

std::string FirstLineOf(const std::string& s) {
    const size_t nl = s.find('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
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

} // namespace Vcs
