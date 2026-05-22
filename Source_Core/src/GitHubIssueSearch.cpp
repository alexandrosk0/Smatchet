#include "GitHubIssueSearch.h"

#include "GitHubClientHelpers.h"
#include "GitHubFetchPlan.h"
#include "GitHubIssueSearchMapping.h"
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

const char* const kPatMissingError = "GitHub PAT not configured (set Preferences > Tracker > GitHub PAT)";

// PR12 — JSON → CachedTicket mapping (incl. PR detection + status encoding +
// [PR] summary prefix) lives in GitHubIssueSearchMapping.cpp so the doctest
// rig can link it without cpr. This TU only owns the cpr-side fetch loop.

// Extract the per-PR detail URL from a list-response item's `pull_request`
// sub-object. Returns empty when the field is absent or malformed.
std::string ExtractPullRequestApiUrl(const nlohmann::json& issue) {
    if (!issue.is_object()) {
        return std::string();
    }
    const auto it = issue.find("pull_request");
    if (it == issue.end() || !it->is_object()) {
        return std::string();
    }
    const auto urlIt = it->find("url");
    if (urlIt == it->end() || !urlIt->is_string()) {
        return std::string();
    }
    return urlIt->get<std::string>();
}

// Build the canonical /pulls/{n} URL from owner/repo/number when the list
// payload didn't carry `pull_request.url` (defensive fallback — happens
// when the response shape changes between API versions).
std::string BuildPullsUrl(const std::string& baseUrl, const std::string& owner, const std::string& repo,
                          const std::string& issueKey) {
    // issueKey is "<owner>/<repo>#<number>" — extract the number suffix.
    const std::size_t hashPos = issueKey.find('#');
    if (hashPos == std::string::npos) {
        return std::string();
    }
    const std::string number = issueKey.substr(hashPos + 1);
    if (number.empty()) {
        return std::string();
    }
    return baseUrl + "/repos/" + owner + "/" + repo + "/pulls/" + number;
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

    const GitHubFetchPlan plan = ComputeGitHubFetchPlan(owner, repo, translated.Query, translated.IsPullRequestQuery);
    const bool repoScoped = plan.repoScoped;
    if (!plan.warning.empty()) {
        AppendOutWarning(outWarning, plan.warning);
    }
    // On the repo-scoped path, GitHub's REST endpoint does not accept a
    // search-qualifier query string — flag the dropped JQL so the user
    // understands why the result set is broader than the JQL suggested.
    if (repoScoped && !jqlQueryOrEmpty.empty()) {
        AppendOutWarning(outWarning, "GitHub repo-scoped fetch ignores JQL filters; result includes all issues "
                                     "in " +
                                         owner + "/" + repo + " (use cross-repo search to apply filters)");
    }

    const cpr::Header headers = BuildGitHubHeaders(pat);
    std::vector<CachedTicket> results;
    // PR12 — parallel list of /pulls/{n} URLs to enrich post-loop. Indexed by
    // position in `results` (only PR rows get an entry; non-PR rows store an
    // empty string so indices line up).
    std::vector<std::string> prDetailUrls;
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
                *outFetchError =
                    std::string("GitHub fetch error (HTTP ") + std::to_string(resp.status_code) + "): " + msg;
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
            // PR12 — /repos/.../issues + /search/issues both include PRs under
            // the issues namespace (they share the issues table). Drop them
            // unless the user's JQL explicitly asked for PRs via `type:pr`.
            if (!plan.includePullRequests && issue.is_object() && issue.contains("pull_request")) {
                continue;
            }
            try {
                CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, owner, repo);
                std::string detailUrl;
                if (plan.includePullRequests && t.fieldValues.count(kIsPullRequestSentinel) != 0) {
                    detailUrl = ExtractPullRequestApiUrl(issue);
                    if (detailUrl.empty()) {
                        detailUrl = BuildPullsUrl(baseUrl, owner, repo, t.id);
                    }
                }
                results.push_back(std::move(t));
                prDetailUrls.push_back(std::move(detailUrl));
            } catch (const std::exception& ex) {
                LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: skipping malformed issue: %s", ex.what());
            }
        }

        if (pageCount < static_cast<std::size_t>(kGitHubPerPage)) {
            reachedShortPage = true;
            break;
        }

        if (page == kGitHubMaxPages) {
            AppendOutWarning(outWarning, std::string("GitHub fetch page cap (") + std::to_string(kGitHubMaxPages) +
                                             ") reached; further results not fetched. Narrow your view JQL to fit.");
            LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: page cap %d reached", kGitHubMaxPages);
        }
    }

    // PR12 — when the user asked for PRs, walk the result set and enrich each
    // PR row with the 4 PR-only fields (pr.head, pr.base, pr.mergeable,
    // pr.draft) via a per-PR GET /repos/{o}/{r}/pulls/{n}. The list/search
    // response only carries `{url, html_url, merged_at}` in the
    // `pull_request` sub-object — branch refs + mergeable + draft come only
    // from the detail endpoint. Cap matches the page cap (kGitHubMaxPages *
    // kGitHubPerPage); on hit emit a Warning rather than silently truncating.
    if (plan.includePullRequests) {
        constexpr std::size_t kEnrichCap =
            static_cast<std::size_t>(kGitHubMaxPages) * static_cast<std::size_t>(kGitHubPerPage);
        std::size_t enriched = 0;
        bool capWarned = false;
        for (std::size_t idx = 0; idx < results.size() && idx < prDetailUrls.size(); ++idx) {
            const std::string& detailUrl = prDetailUrls[idx];
            if (detailUrl.empty()) {
                continue; // non-PR row
            }
            if (enriched >= kEnrichCap) {
                if (!capWarned) {
                    AppendOutWarning(outWarning, std::string("GitHub PR-enrichment cap (") +
                                                     std::to_string(kEnrichCap) +
                                                     ") reached; further PR rows left without pr.* "
                                                     "fields. Narrow your view JQL to fit.");
                    LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: PR enrichment cap %zu reached", kEnrichCap);
                    capWarned = true;
                }
                continue;
            }
            const cpr::Response prResp = TrackerGetLogged("GitHubClient", detailUrl, headers,
                                                          kGitHubFetchConnectTimeoutMs, kGitHubFetchOverallTimeoutMs);
            if (prResp.status_code != 200) {
                LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: PR enrichment HTTP %ld on %s — leaving pr.* fields "
                         "empty",
                         prResp.status_code, detailUrl.c_str());
                ++enriched;
                continue;
            }
            nlohmann::json prDetail = nlohmann::json::parse(prResp.text, nullptr, false);
            if (prDetail.is_discarded()) {
                LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: PR enrichment invalid JSON for %s",
                         detailUrl.c_str());
                ++enriched;
                continue;
            }
            try {
                EnrichPullRequestFieldsFromJson(results[idx], prDetail);
            } catch (const std::exception& ex) {
                LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: PR enrichment failed for %s: %s", detailUrl.c_str(),
                         ex.what());
            }
            ++enriched;
        }
        LOG_INFO("GitHubIssueSearch::FetchIssuesViaRestApi: enriched %zu PR rows", enriched);
    }

    // Strip the internal sentinel before returning — callers see PR rows via
    // the `[PR] ` summary prefix and the `pr.*` columns, not the sentinel.
    for (CachedTicket& t : results) {
        t.fieldValues.erase(kIsPullRequestSentinel);
    }

    if (outFullSyncCompleted) {
        *outFullSyncCompleted = reachedShortPage;
    }
    LOG_INFO("GitHubIssueSearch::FetchIssuesViaRestApi: fetched %zu issues (repoScoped=%d, includePRs=%d)",
             results.size(), repoScoped ? 1 : 0, plan.includePullRequests ? 1 : 0);
    return results;
}

bool FetchIssuesForKeysViaRestApi(const std::string& baseUrl, const std::string& pat,
                                  const std::vector<std::string>& issueKeys, std::vector<CachedTicket>& outTickets,
                                  std::string& outError) {
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
        const std::string url =
            baseUrl + "/repos/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + std::to_string(parsed.Number);
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
            CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(parsed_json, parsed.Owner, parsed.Repo);
            // Single-issue path doesn't currently enrich PR rows (callers are
            // FetchIssuesForKeys which works on issue keys); strip the
            // sentinel so it doesn't leak into the cache.
            t.fieldValues.erase(kIsPullRequestSentinel);
            outTickets.push_back(std::move(t));
        } catch (const std::exception& ex) {
            outError = std::string("Failed to parse issue ") + key + ": " + ex.what();
            return false;
        }
    }
    return true;
}

} // namespace github
} // namespace smatchet
