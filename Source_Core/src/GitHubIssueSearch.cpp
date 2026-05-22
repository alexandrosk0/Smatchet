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

// PR4 of docs/design/github-tracker-backend.md — paginated fetcher.
// PR12 (Strategy C revision) — replaced REST N+1 (paginated /search/issues +
// per-PR GET /pulls/{n}) with one GraphQL query per page. Same 10-page cap;
// PR-only fields arrive inline in the same response via PullRequest fragment.

namespace smatchet {
namespace github {

namespace {

// 30s overall budget for issue fetching (vs probe's 5s) — accommodates
// realistic per-page response times on cold backends + the 10-page worst case.
constexpr long kGitHubFetchConnectTimeoutMs = 5000;
constexpr long kGitHubFetchOverallTimeoutMs = 30000;

// GitHub GraphQL search caps at 100 nodes/page (mirrors REST per_page=100).
// We cap at 10 pages = 1000 items, same ceiling as the GitHub REST search
// API hard limit. Surface a Warning when reached.
constexpr int kGitHubPerPage = 100;
constexpr int kGitHubMaxPages = 10;

const char* const kPatMissingError = "GitHub PAT not configured (set Preferences > Tracker > GitHub PAT)";

// Resolve the GraphQL endpoint from the REST base URL.
//   - "https://api.github.com"     → "https://api.github.com/graphql"
//   - "https://<host>/api/v3"      → "https://<host>/api/graphql"
//
// GHE convention: REST lives at `/api/v3`, GraphQL lives at `/api/graphql`.
std::string ResolveGraphQlEndpoint(const std::string& baseUrl) {
    const std::string kApiV3Suffix = "/api/v3";
    if (baseUrl.size() > kApiV3Suffix.size() &&
        baseUrl.compare(baseUrl.size() - kApiV3Suffix.size(), kApiV3Suffix.size(), kApiV3Suffix) == 0) {
        return baseUrl.substr(0, baseUrl.size() - kApiV3Suffix.size()) + "/api/graphql";
    }
    return baseUrl + "/graphql";
}

// The GraphQL document. Inline `search()` so we get both Issues and PRs in
// one round-trip. PullRequest fragment carries the 4 PR-only fields PR12
// surfaces in the grid (headRefName, baseRefName, mergeable, isDraft) plus
// mergedAt for the merged-PR status discriminator.
const char* const kGraphQlSearchQuery = R"GQL(
query($q: String!, $first: Int!, $after: String) {
  search(query: $q, type: ISSUE, first: $first, after: $after) {
    issueCount
    pageInfo { hasNextPage endCursor }
    nodes {
      __typename
      ... on Issue {
        number
        title
        state
        body
        createdAt
        updatedAt
        author { login }
        assignees(first: 1) { nodes { login } }
        labels(first: 50) { nodes { name } }
        milestone { title }
        repository { name owner { login } }
      }
      ... on PullRequest {
        number
        title
        state
        body
        createdAt
        updatedAt
        mergedAt
        headRefName
        baseRefName
        mergeable
        isDraft
        author { login }
        assignees(first: 1) { nodes { login } }
        labels(first: 50) { nodes { name } }
        milestone { title }
        repository { name owner { login } }
      }
    }
  }
}
)GQL";

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

// Build the JSON body for one GraphQL search page.
//   - effectiveQuery is the translated JQL search string (same syntax as REST
//     /search/issues — GraphQL `search()` accepts the same qualifiers).
//   - afterCursor is empty on page 1; subsequent pages use the previous
//     pageInfo.endCursor.
std::string BuildGraphQlBody(const std::string& effectiveQuery, const std::string& afterCursor) {
    nlohmann::json variables = nlohmann::json::object();
    variables["q"] = effectiveQuery;
    variables["first"] = kGitHubPerPage;
    if (afterCursor.empty()) {
        variables["after"] = nullptr;
    } else {
        variables["after"] = afterCursor;
    }
    nlohmann::json body = nlohmann::json::object();
    body["query"] = kGraphQlSearchQuery;
    body["variables"] = variables;
    return body.dump();
}

// Flatten `parsed.errors[]` (when present) into a single human-readable string.
// Returns empty when no errors. GraphQL HTTP responses can be 200 OK with a
// non-empty `errors` array — we must surface that as a fatal fetch error.
std::string ExtractGraphQlErrors(const nlohmann::json& parsed) {
    if (!parsed.is_object()) {
        return std::string();
    }
    const auto it = parsed.find("errors");
    if (it == parsed.end() || !it->is_array() || it->empty()) {
        return std::string();
    }
    std::string out;
    for (const auto& err : *it) {
        if (!err.is_object()) {
            continue;
        }
        const auto msgIt = err.find("message");
        if (msgIt == err.end() || !msgIt->is_string()) {
            continue;
        }
        if (!out.empty()) {
            out.append("; ");
        }
        out.append(msgIt->get<std::string>());
    }
    return out;
}

} // namespace

std::vector<CachedTicket> FetchIssuesViaRestApi(const std::string& baseUrl, const std::string& pat,
                                                const std::string& owner, const std::string& repo,
                                                const std::string& jqlQueryOrEmpty, bool* outFullSyncCompleted,
                                                std::string* outFetchError, std::string* outWarning) {
    return FetchIssuesViaRestApi(baseUrl, pat, owner, repo, jqlQueryOrEmpty, outFullSyncCompleted, outFetchError,
                                 outWarning, nullptr);
}

std::vector<CachedTicket> FetchIssuesViaRestApi(
    const std::string& baseUrl, const std::string& pat, const std::string& owner, const std::string& repo,
    const std::string& jqlQueryOrEmpty, bool* outFullSyncCompleted, std::string* outFetchError, std::string* outWarning,
    const std::function<void(const std::vector<CachedTicket>& page, bool isLast)>& onPage) {
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
    if (!plan.warning.empty()) {
        AppendOutWarning(outWarning, plan.warning);
    }

    // GraphQL path always uses `search()` with the same qualifier syntax as
    // REST /search/issues. The fetch plan was built for the REST split (its
    // `effectiveQuery` is empty on the repo-scoped path because REST
    // /repos/{o}/{r}/issues doesn't accept `q=`); GraphQL accepts the same
    // qualifier syntax universally, so we prefer `translated.Query` (which
    // already carries the `repo:owner/repo` prefix when configured) and only
    // fall back to `plan.effectiveQuery` for the cross-repo no-JQL case where
    // it carries the "is:issue is:open" fallback.
    //
    // Repo-scoped JQL is honoured here (GraphQL search accepts the qualifiers
    // REST /repos/{o}/{r}/issues couldn't). Strategy C deviation from PR12
    // plan, documented in the plan-doc § Deviations.
    std::string graphQlQuery = translated.Query.empty() ? plan.effectiveQuery : translated.Query;
    if (graphQlQuery.empty()) {
        graphQlQuery = "is:issue is:open";
    }
    const std::string endpoint = ResolveGraphQlEndpoint(baseUrl);
    const cpr::Header headers = BuildGitHubHeaders(pat);

    // Per AGENTS.md: nlohmann json `obj["k"] = v`, not brace-list reassignment.
    // BuildGraphQlBody already follows this convention.

    std::vector<CachedTicket> results;
    bool reachedShortPage = false;
    std::string cursor; // empty → page 1; subsequent pages use endCursor.
    bool emittedTerminal = false;
    // Helper: fire onPage with an empty terminal page after a mid-stream
    // error. Idempotent — only fires when no prior emission carried isLast.
    auto emitTerminalIfNeeded = [&]() {
        if (onPage && !emittedTerminal) {
            std::vector<CachedTicket> empty;
            onPage(empty, /*isLast*/ true);
            emittedTerminal = true;
        }
    };

    for (int page = 1; page <= kGitHubMaxPages; ++page) {
        const std::string body = BuildGraphQlBody(graphQlQuery, cursor);
        const cpr::Response resp = TrackerPostLogged("GitHubClient", endpoint, headers, body);

        if (resp.status_code != 200) {
            const std::string msg = ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
            if (outFetchError) {
                *outFetchError =
                    std::string("GitHub fetch error (HTTP ") + std::to_string(resp.status_code) + "): " + msg;
            }
            LOG_ERROR("GitHubIssueSearch::FetchIssuesViaRestApi GraphQL HTTP %ld on page %d: %s", resp.status_code,
                      page, msg.c_str());
            emitTerminalIfNeeded();
            return results;
        }

        nlohmann::json parsed = nlohmann::json::parse(resp.text, nullptr, false);
        if (parsed.is_discarded()) {
            if (outFetchError) {
                *outFetchError = "GitHub returned invalid JSON";
            }
            LOG_ERROR("GitHubIssueSearch::FetchIssuesViaRestApi GraphQL invalid JSON on page %d", page);
            emitTerminalIfNeeded();
            return results;
        }

        // GraphQL responses can return HTTP 200 + a populated `errors` array
        // (partial failure / rate-limit warning / bad query). Treat any
        // non-empty `errors` as fatal.
        const std::string gqlErrors = ExtractGraphQlErrors(parsed);
        if (!gqlErrors.empty()) {
            if (outFetchError) {
                *outFetchError = std::string("GitHub GraphQL error: ") + gqlErrors;
            }
            LOG_ERROR("GitHubIssueSearch::FetchIssuesViaRestApi GraphQL errors on page %d: %s", page, gqlErrors.c_str());
            emitTerminalIfNeeded();
            return results;
        }

        if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
            !parsed["data"].contains("search") || !parsed["data"]["search"].is_object()) {
            if (outFetchError) {
                *outFetchError = "GitHub GraphQL response missing data.search";
            }
            LOG_ERROR("GitHubIssueSearch::FetchIssuesViaRestApi GraphQL response missing data.search on page %d", page);
            emitTerminalIfNeeded();
            return results;
        }

        const nlohmann::json& search = parsed["data"]["search"];
        const nlohmann::json& nodes = search.value("nodes", nlohmann::json::array());
        if (!nodes.is_array()) {
            if (outFetchError) {
                *outFetchError = "GitHub GraphQL search.nodes not an array";
            }
            emitTerminalIfNeeded();
            return results;
        }

        // Map this page's nodes — pure-logic helper (no HTTP, sentinel stripped).
        std::vector<CachedTicket> pageTickets =
            MapGraphQlNodesToTickets(nodes, owner, repo, plan.includePullRequests);

        // Determine isLast BEFORE recording the next-cursor — the per-page
        // callback contract requires `isLast == true` on exactly one
        // invocation (the terminal page, regardless of why we stopped).
        const nlohmann::json& pageInfo = search.value("pageInfo", nlohmann::json::object());
        // Null-safe extraction: nlohmann's .value() default only fires on missing
        // key, NOT on JSON null. GitHub returns `endCursor: null` when the result
        // set is empty (e.g. issueCount=0), which would throw type_error.302.
        bool hasNext = false;
        {
            const auto it = pageInfo.find("hasNextPage");
            if (it != pageInfo.end() && it->is_boolean()) {
                hasNext = it->get<bool>();
            }
        }
        std::string endCursor;
        {
            const auto it = pageInfo.find("endCursor");
            if (it != pageInfo.end() && it->is_string()) {
                endCursor = it->get<std::string>();
            }
        }

        bool isLastPage = false;
        bool stopLoop = false;
        if (!hasNext) {
            reachedShortPage = true;
            isLastPage = true;
            stopLoop = true;
        } else if (page == kGitHubMaxPages) {
            AppendOutWarning(outWarning, std::string("GitHub fetch page cap (") + std::to_string(kGitHubMaxPages) +
                                             ") reached; further results not fetched. Narrow your view JQL to fit.");
            LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: page cap %d reached", kGitHubMaxPages);
            isLastPage = true;
            stopLoop = true;
        } else if (endCursor.empty()) {
            // Defensive — hasNext=true but endCursor empty would loop forever.
            reachedShortPage = true;
            isLastPage = true;
            stopLoop = true;
        }

        LOG_INFO("GitHubIssueSearch::FetchIssuesViaRestApi: page %d mapped=%zu isLast=%d", page, pageTickets.size(),
                 isLastPage ? 1 : 0);

        // Emit this page to the streaming callback (when provided) BEFORE
        // accumulating into `results`. Per-page emission is what lets the UI
        // grid populate progressively (t+1.7s, t+3.4s, ...) instead of
        // all-at-once after the last page returns.
        if (onPage) {
            onPage(pageTickets, isLastPage);
            if (isLastPage) {
                emittedTerminal = true;
            }
        }

        // Accumulate into the legacy return vector — non-streaming callers
        // (tests, FetchIssues backward-compat) still want the all-at-once
        // snapshot.
        results.insert(results.end(), std::make_move_iterator(pageTickets.begin()),
                       std::make_move_iterator(pageTickets.end()));

        if (stopLoop) {
            break;
        }

        cursor = endCursor;
    }

    if (outFullSyncCompleted) {
        *outFullSyncCompleted = reachedShortPage;
    }
    LOG_INFO("GitHubIssueSearch::FetchIssuesViaRestApi: fetched %zu issues via GraphQL (includePRs=%d)",
             results.size(), plan.includePullRequests ? 1 : 0);
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
