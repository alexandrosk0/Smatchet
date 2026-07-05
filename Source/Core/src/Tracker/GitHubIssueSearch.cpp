#include "GitHubIssueSearch.h"

#include "GitHubClientHelpers.h"
#include "GitHubFetchPlan.h"
#include "GitHubIssueSearchMapping.h"
#include "GitHubQueryFromJql.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// docs/plans/shipped/github-tracker-backend.md — paginated fetcher.
// Strategy C revision — replaced REST N+1 (paginated /search/issues +
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
// one round-trip. PullRequest fragment carries the 4 PR-only fields
// surfaced in the grid (headRefName, baseRefName, mergeable, isDraft) plus
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
        comments { totalCount }
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
        comments { totalCount }
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

// Outcome of parsing one GraphQL search-page HTTP response. `Fatal` true means
// the page is unusable (sets `Error`); otherwise `Tickets` + paging fields are
// populated. This is the bucket-A-testable seam — no I/O, pure JSON → struct.
struct GraphQlPageParse {
    bool Fatal = false;
    std::string Error;
    std::vector<CachedTicket> Tickets;
    bool HasNext = false;
    std::string EndCursor;
};

// Parse one GraphQL search-page HTTP response body into a GraphQlPageParse.
// Covers: invalid JSON, GraphQL `errors[]`, missing `data.search`, non-array
// `nodes`. On any of those sets `Fatal` + `Error`. Pure — unit-testable without
// a live backend.
GraphQlPageParse ParseGraphQlSearchPage(const std::string& responseText, const std::string& owner,
                                        const std::string& repo, bool includePullRequests) {
    GraphQlPageParse out;

    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(responseText);
    if (parsed.is_discarded()) {
        out.Fatal = true;
        out.Error = "GitHub returned invalid JSON";
        return out;
    }

    const std::string gqlErrors = ExtractGraphQlErrors(parsed);
    if (!gqlErrors.empty()) {
        out.Fatal = true;
        out.Error = std::string("GitHub GraphQL error: ") + gqlErrors;
        return out;
    }

    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("search") || !parsed["data"]["search"].is_object()) {
        out.Fatal = true;
        out.Error = "GitHub GraphQL response missing data.search";
        return out;
    }

    const nlohmann::json& search = parsed["data"]["search"];
    const nlohmann::json& nodes = search.value("nodes", nlohmann::json::array());
    if (!nodes.is_array()) {
        out.Fatal = true;
        out.Error = "GitHub GraphQL search.nodes not an array";
        return out;
    }

    out.Tickets = MapGraphQlNodesToTickets(nodes, owner, repo, includePullRequests);

    const nlohmann::json& pageInfo = search.value("pageInfo", nlohmann::json::object());
    {
        const auto it = pageInfo.find("hasNextPage");
        if (it != pageInfo.end() && it->is_boolean()) {
            out.HasNext = it->get<bool>();
        }
    }
    {
        const auto it = pageInfo.find("endCursor");
        if (it != pageInfo.end() && it->is_string()) {
            out.EndCursor = it->get<std::string>();
        }
    }
    return out;
}

// github-commit-tracker-rows — run the GraphQL issue/PR search loop. Appends
// mapped tickets to `accum` and emits each page via `emitPage(page, isLast)`.
// `isFinalSource` controls whether this source owns the terminal `isLast`
// emission (false when a commit fetch will run afterwards). Returns
// fullSyncCompleted; sets `*outFatal` true + `*outFetchError` on a fatal error.
bool RunGraphQlIssueSearch(const std::string& endpoint, const cpr::Header& headers, const std::string& graphQlQuery,
                           const std::string& owner, const std::string& repo, bool includePullRequests,
                           bool isFinalSource, std::vector<CachedTicket>& accum,
                           const std::function<void(const std::vector<CachedTicket>&, bool)>& emitPage,
                           std::string* outFetchError, std::string* outWarning, bool* outFatal) {
    if (outFatal) {
        *outFatal = false;
    }
    bool reachedShortPage = false;
    std::string cursor; // empty → page 1; subsequent pages use endCursor.

    for (int page = 1; page <= kGitHubMaxPages; ++page) {
        const std::string body = BuildGraphQlBody(graphQlQuery, cursor);
        const cpr::Response resp = TrackerPostLogged("GitHubClient", endpoint, headers, body);

        if (resp.status_code != 200) {
            const std::string msg = ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
            if (outFetchError) {
                *outFetchError =
                    std::string("GitHub fetch error (HTTP ") + std::to_string(resp.status_code) + "): " + msg;
            }
            LOG_ERROR("GitHubIssueSearch::RunGraphQlIssueSearch GraphQL HTTP %ld on page %d: %s", resp.status_code,
                      page, msg.c_str());
            if (outFatal) {
                *outFatal = true;
            }
            return false;
        }

        GraphQlPageParse parse = ParseGraphQlSearchPage(resp.text, owner, repo, includePullRequests);
        if (parse.Fatal) {
            if (outFetchError) {
                *outFetchError = parse.Error;
            }
            LOG_ERROR("GitHubIssueSearch::RunGraphQlIssueSearch parse failure on page %d: %s", page,
                      parse.Error.c_str());
            if (outFatal) {
                *outFatal = true;
            }
            return false;
        }

        std::vector<CachedTicket> pageTickets = std::move(parse.Tickets);
        const bool hasNext = parse.HasNext;
        const std::string endCursor = parse.EndCursor;

        bool isLastPage = false;
        bool stopLoop = false;
        if (!hasNext) {
            reachedShortPage = true;
            isLastPage = true;
            stopLoop = true;
        } else if (page == kGitHubMaxPages) {
            AppendOutWarning(outWarning, std::string("GitHub fetch page cap (") + std::to_string(kGitHubMaxPages) +
                                             ") reached; further results not fetched. Narrow your view JQL to fit.");
            LOG_WARN("GitHubIssueSearch::RunGraphQlIssueSearch: page cap %d reached", kGitHubMaxPages);
            isLastPage = true;
            stopLoop = true;
        } else if (endCursor.empty()) {
            AppendOutWarning(outWarning,
                             "GitHub GraphQL pageInfo.endCursor missing while hasNextPage=true; stopped early.");
            LOG_WARN("GitHubIssueSearch::RunGraphQlIssueSearch: empty endCursor with hasNextPage=true; "
                     "stopping page %d without claiming full sync",
                     page);
            isLastPage = true;
            stopLoop = true;
        }

        LOG_INFO("GitHubIssueSearch::RunGraphQlIssueSearch: page %d mapped=%zu isLast=%d", page, pageTickets.size(),
                 isLastPage ? 1 : 0);

        emitPage(pageTickets, isLastPage && isFinalSource);

        accum.insert(accum.end(), std::make_move_iterator(pageTickets.begin()),
                     std::make_move_iterator(pageTickets.end()));

        if (stopLoop) {
            break;
        }
        cursor = endCursor;
    }
    return reachedShortPage;
}

// github-commit-tracker-rows — fetch the most-recent page (≤100) of commits for
// the configured repo via REST `/repos/{o}/{r}/commits`. Appends mapped commit
// rows to `accum` + emits a single page via `emitPage`. Commit-fetch failure is
// non-fatal: it appends a warning and returns false (fullSync not completed) so
// the caller propagates FullSyncCompleted=false and cached rows aren't pruned.
bool RunCommitFetch(const std::string& baseUrl, const cpr::Header& headers, const std::string& owner,
                    const std::string& repo, bool isFinalSource, std::vector<CachedTicket>& accum,
                    const std::function<void(const std::vector<CachedTicket>&, bool)>& emitPage,
                    std::string* outWarning) {
    const std::string url = baseUrl + BuildCommitListUrlSuffix(owner, repo, kGitHubPerPage);
    const cpr::Response resp =
        TrackerGetLogged("GitHubClient", url, headers, kGitHubFetchConnectTimeoutMs, kGitHubFetchOverallTimeoutMs);

    if (resp.status_code != 200) {
        const std::string msg = ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
        AppendOutWarning(outWarning, std::string("GitHub commit fetch failed (HTTP ") +
                                         std::to_string(resp.status_code) + "): " + msg +
                                         " — cached commit rows preserved.");
        LOG_ERROR("GitHubIssueSearch::RunCommitFetch HTTP %ld: %s", resp.status_code, msg.c_str());
        const std::vector<CachedTicket> empty;
        emitPage(empty, isFinalSource);
        return false;
    }

    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    if (parsed.is_discarded() || !parsed.is_array()) {
        AppendOutWarning(outWarning, "GitHub commit fetch returned invalid JSON — cached commit rows preserved.");
        LOG_ERROR("GitHubIssueSearch::RunCommitFetch invalid JSON / not an array");
        const std::vector<CachedTicket> empty;
        emitPage(empty, isFinalSource);
        return false;
    }

    std::vector<CachedTicket> pageTickets;
    pageTickets.reserve(parsed.size());
    for (const auto& c : parsed) {
        if (!c.is_object()) {
            continue;
        }
        try {
            pageTickets.push_back(MapCommitJsonToCachedTicket(c, owner, repo));
        } catch (const std::exception&) {
            // Malformed commit — skip silently (Pillar 3). Aggregate count
            // below is enough for diagnostics.
        }
    }
    LOG_INFO("GitHubIssueSearch::RunCommitFetch: fetched %zu commit rows", pageTickets.size());

    emitPage(pageTickets, isFinalSource);
    accum.insert(accum.end(), std::make_move_iterator(pageTickets.begin()), std::make_move_iterator(pageTickets.end()));
    return true;
}

} // namespace

std::vector<CachedTicket> FetchIssuesViaRestApi(const std::string& baseUrl, const std::string& pat,
                                                const std::string& owner, const std::string& repo,
                                                const std::string& jqlQueryOrEmpty, bool* outFullSyncCompleted,
                                                std::string* outFetchError, std::string* outWarning) {
    return FetchIssuesViaRestApi(baseUrl, pat, owner, repo, jqlQueryOrEmpty, outFullSyncCompleted, outFetchError,
                                 outWarning, nullptr);
}

namespace {

// Result of translating JQL into the GraphQL fetch shape: the resolved query
// string, the source-run flags from the fetch plan, and whether translation
// failed. On failure the error is already written to the fetch-error out param.
struct GitHubFetchSetup {
    bool fatal = false;
    std::string graphQlQuery;
    bool includePullRequests = false;
    bool includeCommits = false;
    bool includeIssuesOrPullRequests = false;
    // Item 18c (user-info-window) — canonical `owner/repo#N` key from a JQL
    // `key = ...` clause; the fetch post-filters rows to this single issue.
    std::string keyFilter;
};

// Translate JQL once and build the GraphQL query + run flags. Warnings are
// non-fatal and appended to `outWarning`; a translation failure sets
// `outFetchError` and returns `fatal = true`.
GitHubFetchSetup BuildGitHubFetchSetup(const std::string& owner, const std::string& repo,
                                       const std::string& jqlQueryOrEmpty, std::string* outFetchError,
                                       std::string* outWarning) {
    GitHubFetchSetup setup;

    const JqlToGitHubResult translated = TranslateJqlToGitHubSearch(jqlQueryOrEmpty, owner, repo);
    if (!translated.Ok) {
        if (outFetchError) {
            *outFetchError = std::string("JQL translation failed: ") + translated.Error;
        }
        setup.fatal = true;
        return setup;
    }
    if (!translated.Warning.empty()) {
        AppendOutWarning(outWarning, translated.Warning);
    }

    const GitHubFetchPlan plan =
        ComputeGitHubFetchPlan(owner, repo, translated.Query, translated.IncludePullRequests, translated.IncludeCommits,
                               translated.IncludeIssuesOrPullRequests);
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
    // Repo-scoped JQL is honoured here (GraphQL search accepts the qualifiers
    // REST /repos/{o}/{r}/issues couldn't). Strategy C deviation from the
    // original plan, documented in the plan-doc § Deviations.
    std::string graphQlQuery = translated.Query.empty() ? plan.effectiveQuery : translated.Query;
    if (graphQlQuery.empty()) {
        graphQlQuery = "is:issue is:open";
    }
    setup.graphQlQuery = std::move(graphQlQuery);
    setup.includePullRequests = plan.includePullRequests;
    setup.includeCommits = plan.includeCommits;
    setup.includeIssuesOrPullRequests = plan.includeIssuesOrPullRequests;
    setup.keyFilter = translated.KeyFilter;
    return setup;
}

// #984 diagnosability — a user query carrying an `is:` qualifier that maps zero
// rows is the silent-drop signature (e.g. `is:pr` returning PRs the mapper
// discards). With the translator fix this should no longer fire for `is:pr`, but
// the breadcrumb stays for any future passthrough/flag mismatch.
void WarnIfIsQualifierMappedZeroRows(const std::string& jqlQueryOrEmpty, const std::string& graphQlQuery,
                                     bool includePullRequests, bool zeroRows) {
    if (zeroRows && jqlQueryOrEmpty.find("is:") != std::string::npos) {
        LOG_WARN("GitHubIssueSearch::FetchIssuesViaRestApi: 0 rows for an is:-qualified query '%s' (includePRs=%d) — "
                 "check that native is: qualifiers set the matching include-flags",
                 graphQlQuery.c_str(), includePullRequests ? 1 : 0);
    }
}

} // namespace

std::vector<CachedTicket>
FetchIssuesViaRestApi(const std::string& baseUrl, const std::string& pat, const std::string& owner,
                      const std::string& repo, const std::string& jqlQueryOrEmpty, bool* outFullSyncCompleted,
                      std::string* outFetchError, std::string* outWarning,
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

    const GitHubFetchSetup setup = BuildGitHubFetchSetup(owner, repo, jqlQueryOrEmpty, outFetchError, outWarning);
    if (setup.fatal) {
        return {};
    }
    const std::string& graphQlQuery = setup.graphQlQuery;
    const std::string endpoint = ResolveGraphQlEndpoint(baseUrl);
    const cpr::Header headers = BuildGitHubHeaders(pat);

    // github-commit-tracker-rows — orchestrate up to two row sources: the
    // GraphQL issue/PR search and the REST commit fetch. The terminal `isLast`
    // emission must fire exactly once total; the final source that runs owns it.
    const bool willRunCommits = setup.includeCommits; // already downgraded if owner/repo missing
    const bool willRunIssues = setup.includeIssuesOrPullRequests;

    std::vector<CachedTicket> results;
    bool emittedTerminal = false;
    const std::string& keyFilter = setup.keyFilter;
    auto emit = [&](const std::vector<CachedTicket>& page, bool isLast) {
        if (!onPage) {
            return;
        }
        // Item 18c — a `key = owner/repo#N` clause narrows each streamed page to
        // that single issue (the mapper writes the canonical key to ticket.id).
        if (!keyFilter.empty()) {
            std::vector<CachedTicket> kept;
            for (const auto& t : page) {
                if (t.id == keyFilter) {
                    kept.push_back(t);
                }
            }
            onPage(kept, isLast);
        } else {
            onPage(page, isLast);
        }
        if (isLast) {
            emittedTerminal = true;
        }
    };
    // Fire a terminal empty page when no source claimed `isLast` (e.g. a fatal
    // issue error, or nothing ran at all). Idempotent.
    auto emitTerminalIfNeeded = [&]() {
        if (onPage && !emittedTerminal) {
            const std::vector<CachedTicket> empty;
            onPage(empty, /*isLast*/ true);
            emittedTerminal = true;
        }
    };

    bool issuesFullSync = true; // vacuously complete when the source isn't run
    bool commitsFullSync = true;

    // Item 18c — the run functions accumulate unfiltered pages into the results
    // vector, so the aggregate return value needs the same single-issue narrowing.
    auto applyKeyFilter = [&]() {
        if (!keyFilter.empty()) {
            results.erase(std::remove_if(results.begin(), results.end(),
                                         [&keyFilter](const CachedTicket& t) { return t.id != keyFilter; }),
                          results.end());
        }
    };

    if (willRunIssues) {
        bool fatal = false;
        issuesFullSync =
            RunGraphQlIssueSearch(endpoint, headers, graphQlQuery, owner, repo, setup.includePullRequests,
                                  /*isFinalSource=*/!willRunCommits, results, emit, outFetchError, outWarning, &fatal);
        if (fatal) {
            emitTerminalIfNeeded();
            if (outFullSyncCompleted) {
                *outFullSyncCompleted = false;
            }
            applyKeyFilter();
            return results;
        }
    }

    if (willRunCommits) {
        commitsFullSync =
            RunCommitFetch(baseUrl, headers, owner, repo, /*isFinalSource=*/true, results, emit, outWarning);
    }

    // Safety net — if neither source emitted a terminal page (e.g. a
    // commits-only view downgraded to no sources), still close the stream.
    emitTerminalIfNeeded();
    applyKeyFilter();

    if (outFullSyncCompleted) {
        // Never claim full sync when nothing ran (e.g. commits-only view with no
        // owner/repo, downgraded to zero sources) — a spurious true would prune
        // every cached row against an empty result.
        const bool anySourceRan = willRunIssues || willRunCommits;
        *outFullSyncCompleted = anySourceRan && issuesFullSync && commitsFullSync;
    }
    LOG_INFO("GitHubIssueSearch::FetchIssuesViaRestApi: fetched %zu rows (issues=%d commits=%d includePRs=%d)",
             results.size(), willRunIssues ? 1 : 0, willRunCommits ? 1 : 0, setup.includePullRequests ? 1 : 0);
    WarnIfIsQualifierMappedZeroRows(jqlQueryOrEmpty, setup.graphQlQuery, setup.includePullRequests, results.empty());
    return results;
}

Result<std::vector<CachedTicket>, TrackerError>
FetchIssuesForKeysViaRestApi(const std::string& baseUrl, const std::string& pat,
                             const std::vector<std::string>& issueKeys) {
    using FetchResult = Result<std::vector<CachedTicket>, TrackerError>;
    std::vector<CachedTicket> outTickets;
    if (issueKeys.empty()) {
        return FetchResult::Ok(std::move(outTickets));
    }
    if (pat.empty()) {
        return FetchResult::Err(TrackerErrorAuth(kPatMissingError));
    }

    const cpr::Header headers = BuildGitHubHeaders(pat);
    for (const std::string& key : issueKeys) {
        ParsedIssueKey parsed;
        if (!ParseGitHubIssueKey(key, parsed)) {
            return FetchResult::Err(
                TrackerErrorInvalidRequest(std::string("Invalid GitHub issue key (expected owner/repo#N): ") + key));
        }
        const std::string url =
            baseUrl + "/repos/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + std::to_string(parsed.Number);
        const cpr::Response resp =
            TrackerGetLogged("GitHubClient", url, headers, kGitHubFetchConnectTimeoutMs, kGitHubFetchOverallTimeoutMs);
        if (resp.status_code != 200) {
            const std::string msg = ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
            const std::string outError = std::string("GitHub fetch error for ") + key + " (HTTP " +
                                         std::to_string(resp.status_code) + "): " + msg;
            LOG_ERROR("GitHubIssueSearch::FetchIssuesForKeysViaRestApi HTTP %ld on %s: %s", resp.status_code,
                      key.c_str(), msg.c_str());
            // Guard the `!= 200` branch before FromHttpStatus (FIX-1): a 2xx-other → Unknown, never a
            // dropped Ok(). Detail preserved verbatim for the caller's text checks.
            if (resp.status_code >= 200 && resp.status_code < 300) {
                return FetchResult::Err(TrackerErrorUnknown(outError, static_cast<int>(resp.status_code)));
            }
            return FetchResult::Err(TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), outError));
        }
        // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
        nlohmann::json parsed_json = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
        if (parsed_json.is_discarded() || !parsed_json.is_object()) {
            return FetchResult::Err(TrackerErrorParse(std::string("Invalid JSON in single-issue response for ") + key));
        }
        try {
            CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(parsed_json, parsed.Owner, parsed.Repo);
            // Single-issue path doesn't currently enrich PR rows (callers are
            // FetchIssuesForKeys which works on issue keys); strip the
            // sentinel so it doesn't leak into the cache.
            t.fieldValues.erase(kIsPullRequestSentinel);
            outTickets.push_back(std::move(t));
        } catch (const std::exception& ex) {
            return FetchResult::Err(TrackerErrorParse(std::string("Failed to parse issue ") + key + ": " + ex.what()));
        }
    }
    return FetchResult::Ok(std::move(outTickets));
}

} // namespace github
} // namespace smatchet
