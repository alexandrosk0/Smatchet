#include "GitHubIssueSearch.h"

#include "GitHubClientHelpers.h"
#include "GitHubFetchPlan.h"
#include "GitHubQueryFromJql.h"
#include "Logger.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

// PR4 of docs/design/github-tracker-backend.md — paginated REST fetcher.
// Lives outside the GitHubClient class (mirrors JiraIssueSearch / PlaneIssueSearch
// convention). Pure consumer of the public helpers; no class state, no globals.

namespace smatchet {
namespace github {

namespace {

// 30s overall budget for issue fetching (vs probe's 5s) — accommodates
// realistic per-page response times on cold backends + the 10-page worst case.
constexpr long kGitHubFetchConnectTimeoutMs = 5000;
constexpr long kGitHubFetchOverallTimeoutMs = 30000;

// GitHub paginates at 100 items/page. We cap at 10 pages for the first slice
// (matches the JiraIssueSearch safety cap + plan note); when reached, surface
// a Warning rather than failing.
constexpr int kGitHubPerPage = 100;
constexpr int kGitHubMaxPages = 10;

// Truncate description bodies above this size — saves memory + log volume.
constexpr std::size_t kGitHubBodyMaxBytes = 4096;

const char* const kPatMissingError = "GitHub PAT not configured (set Preferences > Tracker > GitHub PAT)";

std::string JsonString(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) {
        return std::string();
    }
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return std::string();
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    // Coerce numbers / bools to string for permissive parsing.
    try {
        return it->dump();
    } catch (...) {
        return std::string();
    }
}

// Pluck the issue number out of `/repos/.../issues/<n>` or the top-level
// "number" field, returning "0" as a fallback (we still write the ticket, but
// the id won't be canonical).
std::string IssueNumberString(const nlohmann::json& issue) {
    if (issue.is_object() && issue.contains("number") && issue["number"].is_number_integer()) {
        return std::to_string(issue["number"].get<std::int64_t>());
    }
    return std::string("0");
}

// Map a single GitHub issue JSON to a CachedTicket. `ownerHint` / `repoHint`
// are used to compose the canonical id when the issue payload doesn't carry
// `repository_url` (single-issue endpoint). The /search/issues + /repos
// endpoints both include `repository_url`, so the hints are only consulted on
// the single-issue fetch path.
CachedTicket MapIssueToCachedTicket(const nlohmann::json& issue, const std::string& ownerHint,
                                     const std::string& repoHint) {
    CachedTicket ticket;
    std::string owner = ownerHint;
    std::string repo = repoHint;

    // Cross-repo path embeds the repo in `repository_url` of the form
    // "https://api.github.com/repos/<owner>/<repo>".
    const std::string repoUrl = JsonString(issue, "repository_url");
    if (!repoUrl.empty()) {
        const std::size_t reposPos = repoUrl.find("/repos/");
        if (reposPos != std::string::npos) {
            const std::string tail = repoUrl.substr(reposPos + 7);
            const std::size_t slash = tail.find('/');
            if (slash != std::string::npos) {
                owner = tail.substr(0, slash);
                repo = tail.substr(slash + 1);
            }
        }
    }

    const std::string number = IssueNumberString(issue);
    if (!owner.empty() && !repo.empty() && number != "0") {
        ticket.id = owner + "/" + repo + "#" + number;
    } else {
        // Fallback shape — best-effort; ParseGitHubIssueKey will reject this
        // but at least the row is visible in the grid.
        ticket.id = std::string("#") + number;
    }
    ticket.fieldValues["key"] = ticket.id;

    ticket.fieldValues["summary"] = JsonString(issue, "title");
    ticket.fieldValues["status"] = JsonString(issue, "state");

    if (issue.is_object() && issue.contains("assignee") && issue["assignee"].is_object()) {
        ticket.fieldValues["assignee"] = JsonString(issue["assignee"], "login");
    } else {
        ticket.fieldValues["assignee"] = std::string();
    }

    if (issue.is_object() && issue.contains("user") && issue["user"].is_object()) {
        ticket.fieldValues["reporter"] = JsonString(issue["user"], "login");
    } else {
        ticket.fieldValues["reporter"] = std::string();
    }

    std::string labelStr;
    if (issue.is_object() && issue.contains("labels") && issue["labels"].is_array()) {
        for (const auto& lbl : issue["labels"]) {
            std::string name;
            if (lbl.is_object()) {
                name = JsonString(lbl, "name");
            } else if (lbl.is_string()) {
                name = lbl.get<std::string>();
            }
            if (name.empty()) {
                continue;
            }
            if (!labelStr.empty()) {
                labelStr.append(", ");
            }
            labelStr.append(name);
        }
    }
    ticket.fieldValues["labels"] = labelStr;

    std::string body = JsonString(issue, "body");
    if (body.size() > kGitHubBodyMaxBytes) {
        LOG_TRACE("GitHubIssueSearch: truncating issue body %zu → %zu bytes for %s", body.size(), kGitHubBodyMaxBytes,
                  ticket.id.c_str());
        body.resize(kGitHubBodyMaxBytes);
    }
    ticket.fieldValues["description"] = body;

    ticket.fieldValues["created"] = JsonString(issue, "created_at");
    ticket.fieldValues["updated"] = JsonString(issue, "updated_at");

    // Milestone — flatten to title string when present.
    if (issue.is_object() && issue.contains("milestone") && issue["milestone"].is_object()) {
        ticket.fieldValues["milestone"] = JsonString(issue["milestone"], "title");
    }

    return ticket;
}

// Build the page URL for the cross-repo /search/issues path.
std::string BuildSearchUrl(const std::string& baseUrl, const std::string& query, int page) {
    std::string url = baseUrl + "/search/issues?q=" + UrlEncode(query);
    url += "&per_page=" + std::to_string(kGitHubPerPage);
    url += "&page=" + std::to_string(page);
    return url;
}

// Build the page URL for the repo-scoped /repos/{o}/{r}/issues path. JQL is
// NOT applied here — the endpoint doesn't accept a `q=` parameter.
std::string BuildRepoIssuesUrl(const std::string& baseUrl, const std::string& owner, const std::string& repo,
                               int page) {
    std::string url = baseUrl + "/repos/" + owner + "/" + repo + "/issues?state=all";
    url += "&per_page=" + std::to_string(kGitHubPerPage);
    url += "&page=" + std::to_string(page);
    return url;
}

// Append `msg` to `*out` (when non-null) with "; " separators between entries.
void AppendOutWarning(std::string* out, const std::string& msg) {
    if (out == nullptr || msg.empty()) {
        return;
    }
    if (!out->empty()) {
        out->append("; ");
    }
    out->append(msg);
}

} // namespace

std::vector<CachedTicket> FetchIssuesViaRestApi(const std::string& baseUrl, const std::string& pat,
                                                 const std::string& owner, const std::string& repo,
                                                 const std::string& jqlQueryOrEmpty, bool* outFullSyncCompleted,
                                                 std::string* outFetchError, std::string* outWarning) {
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = false;
    }

    if (pat.empty()) {
        if (outFetchError) {
            *outFetchError = kPatMissingError;
        }
        return {};
    }

    // Translate JQL once; warnings are non-fatal.
    const JqlToGitHubResult translated = TranslateJqlToGitHubSearch(jqlQueryOrEmpty, owner, repo);
    if (!translated.Ok) {
        if (outFetchError) {
            *outFetchError = std::string("JQL translation failed: ") + translated.Error;
        }
        return {};
    }
    if (!translated.Warning.empty()) {
        AppendOutWarning(outWarning, translated.Warning);
    }

    const GitHubFetchPlan plan = ComputeGitHubFetchPlan(owner, repo, translated.Query);
    const bool repoScoped = plan.repoScoped;
    if (!plan.warning.empty()) {
        AppendOutWarning(outWarning, plan.warning);
    }
    // On the repo-scoped path, GitHub's REST endpoint does not accept a
    // search-qualifier query string — flag the dropped JQL so the user
    // understands why the result set is broader than the JQL suggested.
    if (repoScoped && !jqlQueryOrEmpty.empty()) {
        AppendOutWarning(outWarning,
                         "GitHub repo-scoped fetch ignores JQL filters; result includes all issues "
                         "in " + owner + "/" + repo + " (use cross-repo search to apply filters)");
    }

    const cpr::Header headers = BuildGitHubHeaders(pat);
    std::vector<CachedTicket> results;
    bool reachedShortPage = false;

    for (int page = 1; page <= kGitHubMaxPages; ++page) {
        std::string url;
        if (repoScoped) {
            url = BuildRepoIssuesUrl(baseUrl, owner, repo, page);
        } else {
            // Cross-repo path; ComputeGitHubFetchPlan already resolved the
            // effective query (with the `is:issue is:open` fallback when both
            // owner+repo+jql empty).
            url = BuildSearchUrl(baseUrl, plan.effectiveQuery, page);
        }

        const cpr::Response resp =
            TrackerGetLogged("GitHubClient", url, headers, kGitHubFetchConnectTimeoutMs, kGitHubFetchOverallTimeoutMs);

        if (resp.status_code != 200) {
            const std::string msg = ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
            if (outFetchError) {
                *outFetchError = std::string("GitHub fetch error (HTTP ") + std::to_string(resp.status_code) +
                                  "): " + msg;
            }
            LOG_ERROR("GitHubIssueSearch::FetchIssuesViaRestApi HTTP %ld on page %d: %s", resp.status_code, page,
                      msg.c_str());
            return results;
        }

        nlohmann::json parsed = nlohmann::json::parse(resp.text, nullptr, false);
        if (parsed.is_discarded()) {
            if (outFetchError) {
                *outFetchError = "GitHub returned invalid JSON";
            }
            LOG_ERROR("GitHubIssueSearch::FetchIssuesViaRestApi: invalid JSON on page %d", page);
            return results;
        }

        // /search/issues wraps results in {"items":[...], "total_count": N};
        // /repos/.../issues returns a bare array.
        const nlohmann::json* items = nullptr;
        if (parsed.is_array()) {
            items = &parsed;
        } else if (parsed.is_object() && parsed.contains("items") && parsed["items"].is_array()) {
            items = &parsed["items"];
        }
        if (items == nullptr) {
            if (outFetchError) {
                *outFetchError = "GitHub response missing items array";
            }
            LOG_ERROR("GitHubIssueSearch::FetchIssuesViaRestApi: response missing items array on page %d", page);
            return results;
        }

        const std::size_t pageCount = items->size();
        for (const auto& issue : *items) {
            // /repos/.../issues includes pull requests under the issues namespace
            // (they all share the issues table). Filter them out — Smatchet's
            // tracker grid is for issues, not PRs.
            if (issue.is_object() && issue.contains("pull_request")) {
                continue;
            }
            try {
                results.push_back(MapIssueToCachedTicket(issue, owner, repo));
            } catch (const std::exception& ex) {
                LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: skipping malformed issue: %s", ex.what());
            }
        }

        if (pageCount < static_cast<std::size_t>(kGitHubPerPage)) {
            reachedShortPage = true;
            break;
        }

        if (page == kGitHubMaxPages) {
            AppendOutWarning(outWarning,
                             std::string("GitHub fetch page cap (") + std::to_string(kGitHubMaxPages) +
                                 ") reached; further results not fetched. Narrow your view JQL to fit.");
            LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: page cap %d reached", kGitHubMaxPages);
        }
    }

    if (outFullSyncCompleted) {
        *outFullSyncCompleted = reachedShortPage;
    }
    LOG_INFO("GitHubIssueSearch::FetchIssuesViaRestApi: fetched %zu issues (repoScoped=%d)", results.size(),
             repoScoped ? 1 : 0);
    return results;
}

bool FetchIssuesForKeysViaRestApi(const std::string& baseUrl, const std::string& pat,
                                   const std::vector<std::string>& issueKeys,
                                   std::vector<CachedTicket>& outTickets, std::string& outError) {
    if (issueKeys.empty()) {
        return true;
    }
    if (pat.empty()) {
        outError = kPatMissingError;
        return false;
    }

    const cpr::Header headers = BuildGitHubHeaders(pat);
    for (const std::string& key : issueKeys) {
        ParsedIssueKey parsed;
        if (!ParseGitHubIssueKey(key, parsed)) {
            outError = std::string("Invalid GitHub issue key (expected owner/repo#N): ") + key;
            return false;
        }
        const std::string url = baseUrl + "/repos/" + parsed.Owner + "/" + parsed.Repo + "/issues/" +
                                std::to_string(parsed.Number);
        const cpr::Response resp =
            TrackerGetLogged("GitHubClient", url, headers, kGitHubFetchConnectTimeoutMs, kGitHubFetchOverallTimeoutMs);
        if (resp.status_code != 200) {
            const std::string msg = ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
            outError = std::string("GitHub fetch error for ") + key + " (HTTP " + std::to_string(resp.status_code) +
                       "): " + msg;
            LOG_ERROR("GitHubIssueSearch::FetchIssuesForKeysViaRestApi HTTP %ld on %s: %s", resp.status_code,
                      key.c_str(), msg.c_str());
            return false;
        }
        nlohmann::json parsed_json = nlohmann::json::parse(resp.text, nullptr, false);
        if (parsed_json.is_discarded() || !parsed_json.is_object()) {
            outError = std::string("Invalid JSON in single-issue response for ") + key;
            return false;
        }
        try {
            outTickets.push_back(MapIssueToCachedTicket(parsed_json, parsed.Owner, parsed.Repo));
        } catch (const std::exception& ex) {
            outError = std::string("Failed to parse issue ") + key + ": " + ex.what();
            return false;
        }
    }
    return true;
}

} // namespace github
} // namespace smatchet
