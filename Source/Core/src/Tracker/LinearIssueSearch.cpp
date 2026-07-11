#include "LinearIssueSearch.h"

#include "LinearClientHelpers.h"
#include "LinearIssueMappingPure.h"
#include "LinearQueryFromJql.h"
#include "Logger.h"
#include "TrackerHttpUtils.h"
#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Slice 2 of docs/plans/linear-tracker-backend.md — cursor-paginated
// Linear GraphQL `issues` fetch. Mirrors GitHubIssueSearch.cpp: a thin
// HTTP-touching adapter over the cpr-free pure helpers (key parse, filter
// translation, node mapping). All GraphQL POSTs route through TrackerPostLogged.

namespace smatchet {
namespace linear {

namespace {

// Linear's default `issues` page is 50; the API caps `first` well above that.
// We page 50/request up to ~20 pages = 1000 issues, then soft-warn (matches the
// GitHub/Plane convention of capping + warning rather than failing).
constexpr int kLinearPerPage = 50;
constexpr int kLinearMaxPages = 20;

// The `issues(filter,first,after)` document — every CachedTicket-relevant field
// the pure mapper reads (LinearIssueMappingPure.cpp), plus the cursor pageInfo.
const char* const kIssuesQuery = R"GQL(
query Issues($filter: IssueFilter, $first: Int, $after: String) {
  issues(filter: $filter, first: $first, after: $after) {
    nodes {
      id
      identifier
      number
      title
      description
      url
      priority
      priorityLabel
      createdAt
      updatedAt
      state { id name type }
      assignee { id name displayName }
      creator { id name displayName }
      labels { nodes { id name } }
      team { id key name }
      project { id name }
      cycle { id number name }
    }
    pageInfo { hasNextPage endCursor }
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

// Lowercase a copy of `s` (ASCII) — cpr header keys arrive mixed-case but the
// pure rate-limit parser keys on lowercase names.
std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

// Log the Linear rate-limit / complexity headers at DEBUG (plan § Context —
// surface x-ratelimit-*/x-complexity in diagnostics). cpr lowercases nothing for
// us, so fold the keys first to feed the cpr-free parser.
void LogRateLimitHeaders(const cpr::Header& headers) {
    std::map<std::string, std::string> lowered;
    for (cpr::Header::const_iterator it = headers.begin(); it != headers.end(); ++it) {
        lowered[ToLowerAscii(it->first)] = it->second;
    }
    const LinearRateLimit rl = ParseLinearRateLimitHeaders(lowered);
    if (!rl.Present()) {
        return;
    }
    LOG_DEBUG("LinearIssueSearch: rate-limit requests=%ld/%ld reset=%lld complexity=%ld/%ld cost=%ld",
              rl.RequestsRemaining, rl.RequestsLimit, static_cast<long long>(rl.RequestsResetUnixSec),
              rl.ComplexityRemaining, rl.ComplexityLimit, rl.Complexity);
}

// Build the team-scope clause: prefer the stable team key, else the team UUID.
// Empty object when neither is configured (the caller still scopes via the
// view-query translation, if any).
nlohmann::json BuildTeamScopeFilter(const std::string& teamId, const std::string& teamKey) {
    nlohmann::json scope = nlohmann::json::object();
    // Build nested objects via chained operator[] (NOT brace-init) — the same
    // idiom LinearQueryFromJql uses — to avoid nlohmann's object-vs-array
    // ambiguity on `{{"eq", value}}`.
    if (!teamKey.empty()) {
        scope["team"]["key"]["eq"] = teamKey;
    } else if (!teamId.empty()) {
        scope["team"]["id"]["eq"] = teamId;
    }
    return scope;
}

// Merge the team scope with the translated view-query filter (+ optional title
// search term) into the single `filter` IssueFilter variable. Keys are
// implicitly AND-ed by Linear; on a key clash the view-query filter wins (it is
// the more specific user intent). Translator warnings are appended to outWarning.
nlohmann::json BuildIssueFilter(const std::string& teamId, const std::string& teamKey, const std::string& viewQuery,
                                std::string* outWarning) {
    nlohmann::json filter = BuildTeamScopeFilter(teamId, teamKey);

    const JqlToLinearResult translated = TranslateJqlToLinearFilter(viewQuery);
    if (!translated.Warning.empty()) {
        AppendOutWarning(outWarning, translated.Warning);
    }
    if (translated.HasFilter()) {
        for (nlohmann::json::const_iterator it = translated.Filter.begin(); it != translated.Filter.end(); ++it) {
            filter[it.key()] = it.value();
        }
    }
    if (!translated.SearchTerm.empty()) {
        filter["title"]["containsIgnoreCase"] = translated.SearchTerm;
    }
    return filter;
}

// Build the GraphQL `variables` object for one issues page. An empty `filter`
// object is sent as JSON null so Linear lists the (team-less) issue set rather
// than rejecting an empty IssueFilter.
nlohmann::json BuildIssuesVariables(const nlohmann::json& filter, const std::string& afterCursor) {
    nlohmann::json variables = nlohmann::json::object();
    if (filter.is_object() && !filter.empty()) {
        variables["filter"] = filter;
    } else {
        variables["filter"] = nullptr;
    }
    variables["first"] = kLinearPerPage;
    if (afterCursor.empty()) {
        variables["after"] = nullptr;
    } else {
        variables["after"] = afterCursor;
    }
    return variables;
}

// Outcome of parsing one issues-page HTTP response. `Fatal` true means the page
// is unusable (sets `Error`); otherwise `Tickets` + paging fields are populated.
// Pure JSON → struct — no I/O.
struct IssuesPageParse {
    bool Fatal = false;
    std::string Error;
    std::vector<CachedTicket> Tickets;
    bool HasNext = false;
    std::string EndCursor;
};

// Parse one issues-page response body. Covers invalid JSON, GraphQL `errors[]`
// (200-with-errors is failure, plan § Context), and a missing/short
// `data.issues.nodes` array. Pure.
IssuesPageParse ParseIssuesPage(int httpStatus, const std::string& responseText) {
    IssuesPageParse out;

    std::string parseErr;
    nlohmann::json parsed = smatchet::json_safe::ParseBounded(responseText, parseErr);
    if (!parseErr.empty()) {
        out.Fatal = true;
        out.Error = "Linear returned invalid JSON";
        return out;
    }
    std::string errorMessage;
    if (httpStatus != 200 || LinearResponseHasErrors(parsed, errorMessage)) {
        out.Fatal = true;
        out.Error = errorMessage.empty() ? ExtractLinearErrorMessage(httpStatus, responseText) : errorMessage;
        return out;
    }
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("issues") || !parsed["data"]["issues"].is_object()) {
        out.Fatal = true;
        out.Error = "Linear GraphQL response missing data.issues";
        return out;
    }

    const nlohmann::json& issues = parsed["data"]["issues"];
    const nlohmann::json& nodes = issues.value("nodes", nlohmann::json::array());
    out.Tickets = MapLinearIssueNodesToTickets(nodes);

    const nlohmann::json& pageInfo = issues.value("pageInfo", nlohmann::json::object());
    {
        nlohmann::json::const_iterator it = pageInfo.find("hasNextPage");
        if (it != pageInfo.end() && it->is_boolean()) {
            out.HasNext = it->get<bool>();
        }
    }
    {
        nlohmann::json::const_iterator it = pageInfo.find("endCursor");
        if (it != pageInfo.end() && it->is_string()) {
            out.EndCursor = it->get<std::string>();
        }
    }
    return out;
}

} // namespace

cpr::Header BuildLinearHeaders(const std::string& apiKey) {
    return cpr::Header{
        {"Authorization", BuildLinearAuthHeaderValue(apiKey)},
        {"Content-Type", "application/json"},
    };
}

std::vector<CachedTicket>
FetchIssuesViaGraphQl(const std::string& apiUrl, const std::string& apiKey, const std::string& teamId,
                      const std::string& teamKey, const std::string& viewQuery, bool* outFullSyncCompleted,
                      std::string* outFetchError, std::string* outWarning,
                      const std::function<void(const std::vector<CachedTicket>& page, bool isLast)>& onPage,
                      TrackerError* outFetchErrorStructured) {
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = false;
    }
    std::vector<CachedTicket> results;
    bool emittedTerminal = false;
    auto emit = [&](const std::vector<CachedTicket>& page, bool isLast) {
        if (onPage) {
            onPage(page, isLast);
        }
        if (isLast) {
            emittedTerminal = true;
        }
    };
    auto emitTerminalIfNeeded = [&]() {
        if (!emittedTerminal) {
            const std::vector<CachedTicket> empty;
            emit(empty, /*isLast*/ true);
        }
    };

    const nlohmann::json filter = BuildIssueFilter(teamId, teamKey, viewQuery, outWarning);
    const cpr::Header headers = BuildLinearHeaders(apiKey);

    bool reachedShortPage = false;
    std::string cursor; // empty → page 1; subsequent pages use endCursor.
    for (int page = 1; page <= kLinearMaxPages; ++page) {
        const nlohmann::json variables = BuildIssuesVariables(filter, cursor);
        const std::string body = BuildGraphQLBody(kIssuesQuery, variables);
        /* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms
        const cpr::Response resp = TrackerPostLogged("LinearClient", apiUrl, headers, body);
        LogRateLimitHeaders(resp.header);

        IssuesPageParse parse = ParseIssuesPage(static_cast<int>(resp.status_code), resp.text);
        if (parse.Fatal) {
            if (outFetchError) {
                *outFetchError = parse.Error;
            }
            if (outFetchErrorStructured) {
                // Status is in scope here: a non-200 classifies by status (2xx-other → Unknown,
                // FIX-1 precedent); a fatal on HTTP 200 is body-shape/parse class.
                if (resp.status_code == 200) {
                    *outFetchErrorStructured = TrackerErrorParse(parse.Error);
                } else if (resp.status_code >= 200 && resp.status_code < 300) {
                    *outFetchErrorStructured = TrackerErrorUnknown(parse.Error, static_cast<int>(resp.status_code));
                } else {
                    *outFetchErrorStructured =
                        TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), parse.Error);
                }
            }
            LOG_ERROR("LinearIssueSearch::FetchIssuesViaGraphQl: page %d failed (HTTP %ld): %s", page, resp.status_code,
                      parse.Error.c_str());
            emitTerminalIfNeeded();
            return results;
        }

        std::vector<CachedTicket> pageTickets = std::move(parse.Tickets);
        bool isLastPage = false;
        bool stopLoop = false;
        if (!parse.HasNext) {
            reachedShortPage = true;
            isLastPage = true;
            stopLoop = true;
        } else if (page == kLinearMaxPages) {
            AppendOutWarning(outWarning, std::string("Linear fetch page cap (") + std::to_string(kLinearMaxPages) +
                                             ") reached; further results not fetched. Narrow your view query to fit.");
            LOG_WARN("LinearIssueSearch::FetchIssuesViaGraphQl: page cap %d reached", kLinearMaxPages);
            isLastPage = true;
            stopLoop = true;
        } else if (parse.EndCursor.empty()) {
            AppendOutWarning(outWarning,
                             "Linear GraphQL pageInfo.endCursor missing while hasNextPage=true; stopped early.");
            LOG_WARN("LinearIssueSearch::FetchIssuesViaGraphQl: empty endCursor with hasNextPage=true on page %d",
                     page);
            isLastPage = true;
            stopLoop = true;
        }

        LOG_INFO("LinearIssueSearch::FetchIssuesViaGraphQl: page %d mapped=%zu isLast=%d", page, pageTickets.size(),
                 isLastPage ? 1 : 0);
        emit(pageTickets, isLastPage);
        results.insert(results.end(), std::make_move_iterator(pageTickets.begin()),
                       std::make_move_iterator(pageTickets.end()));

        if (stopLoop) {
            break;
        }
        cursor = parse.EndCursor;
    }

    emitTerminalIfNeeded();
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = reachedShortPage;
    }
    LOG_INFO("LinearIssueSearch::FetchIssuesViaGraphQl: fetched %zu issues (fullSync=%d)", results.size(),
             reachedShortPage ? 1 : 0);
    return results;
}

Result<std::vector<CachedTicket>, TrackerError>
FetchIssuesForKeysViaGraphQl(const std::string& apiUrl, const std::string& apiKey,
                             const std::vector<std::string>& issueKeys) {
    using FetchResult = Result<std::vector<CachedTicket>, TrackerError>;
    std::vector<CachedTicket> outTickets;
    if (issueKeys.empty()) {
        return FetchResult::Ok(std::move(outTickets));
    }

    // Build an or:[{team,number}, ...] disjunction so all keys resolve in one
    // round-trip. A key that doesn't parse as TEAM-123 is a caller bug → Err.
    nlohmann::json orClauses = nlohmann::json::array();
    for (std::size_t i = 0; i < issueKeys.size(); ++i) {
        ParsedLinearIssueKey parsed;
        if (!ParseLinearIssueKey(issueKeys[i], parsed)) {
            return FetchResult::Err(TrackerErrorInvalidRequest(
                std::string("Invalid Linear issue key (expected TEAM-123): ") + issueKeys[i]));
        }
        nlohmann::json clause = nlohmann::json::object();
        clause["team"]["key"]["eq"] = parsed.TeamKey;
        clause["number"]["eq"] = parsed.Number;
        orClauses.push_back(clause);
    }
    nlohmann::json filter = nlohmann::json::object();
    filter["or"] = orClauses;

    // `first` covers the disjunction in one page; clamp to Linear's 250/page
    // ceiling so an oversized key list (well past any real use) isn't rejected.
    int firstCount = static_cast<int>(issueKeys.size());
    if (firstCount > 250) {
        firstCount = 250;
    }
    nlohmann::json variables = nlohmann::json::object();
    variables["filter"] = filter;
    variables["first"] = firstCount;
    variables["after"] = nullptr;
    const std::string body = BuildGraphQLBody(kIssuesQuery, variables);

    const cpr::Header headers = BuildLinearHeaders(apiKey);
    /* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms
    const cpr::Response resp = TrackerPostLogged("LinearClient", apiUrl, headers, body);
    LogRateLimitHeaders(resp.header);

    IssuesPageParse parse = ParseIssuesPage(static_cast<int>(resp.status_code), resp.text);
    if (parse.Fatal) {
        LOG_ERROR("LinearIssueSearch::FetchIssuesForKeysViaGraphQl: HTTP %ld — %s", resp.status_code,
                  parse.Error.c_str());
        // A fatal parse on a 2xx (200-with-GraphQL-errors, or a 2xx-non-200) would
        // classify as Ok() via TrackerErrorFromHttpStatus (Kind==None, detail
        // discarded), swallowing the failure. Carry the detail under a non-OK kind (DR20).
        if (resp.status_code >= 200 && resp.status_code < 300) {
            return FetchResult::Err(TrackerErrorUnknown(parse.Error, static_cast<int>(resp.status_code)));
        }
        return FetchResult::Err(TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), parse.Error));
    }
    outTickets = std::move(parse.Tickets);
    LOG_INFO("LinearIssueSearch::FetchIssuesForKeysViaGraphQl: %zu keys → %zu tickets", issueKeys.size(),
             outTickets.size());
    return FetchResult::Ok(std::move(outTickets));
}

} // namespace linear
} // namespace smatchet
