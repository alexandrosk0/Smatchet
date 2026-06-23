#ifndef SMATCHET_LINEAR_CLIENT_HELPERS_H
#define SMATCHET_LINEAR_CLIENT_HELPERS_H

#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "SmatchetResult.h"

// LinearClientHelpers — pure (cpr-free) helpers consumed by LinearClient, split
// out like GitHubClientHelpers so the doctest rig links them without a network
// stack. Linear's API is GraphQL (POST); personal API keys use a RAW Authorization
// header (no "Bearer"), and errors arrive in a top-level `errors` array even on
// HTTP 200. See docs/plans/linear-tracker-backend.md § Context.

namespace smatchet {
namespace linear {

/// Default Linear GraphQL endpoint, used when no API URL is configured.
extern const char* const kDefaultLinearApiUrl; // "https://api.linear.app/graphql"

/// Parsed `TEAM-123` issue identifier (Linear's `Issue.identifier`). Fields are
/// populated only on success; callers must check the return value of
/// `ParseLinearIssueKey` before reading.
struct ParsedLinearIssueKey {
    std::string TeamKey;        // e.g. "ENG"
    std::int64_t Number = 0;    // e.g. 123
};

/// Parse a Linear issue identifier of the form `TEAM-123` (e.g. `ENG-42`).
/// Strict — a team key of one or more ASCII letters/digits starting with a
/// letter, a single `-`, then a positive integer. Returns false + leaves `out`
/// unchanged on any mismatch. The team key doubles as the draft-scope prefix
/// (Linear's draft scope is the Team — see plan § Approach), mirroring how
/// Jira's project prefix is recovered from `PROJ-123`.
bool ParseLinearIssueKey(const std::string& issueKey, ParsedLinearIssueKey& out);

/// Inverse of `ParseLinearIssueKey` — compose `TEAM-123` from its parts.
std::string FormatLinearIssueKey(const std::string& teamKey, std::int64_t number);

/// Normalize a configured Linear API URL: an empty input returns
/// `kDefaultLinearApiUrl`; otherwise trailing slashes are stripped. Does not
/// validate the scheme (see `IsValidLinearApiUrl`).
std::string NormalizeLinearApiUrl(const std::string& apiUrl);

/// Validate a Linear API URL. Empty is accepted (the default endpoint is used).
/// A non-empty URL must be `https://...`; `http://` and non-URL text are
/// rejected. `Ok(true)` when valid/acceptable; `Err(msg)` on reject.
Result<bool, std::string> IsValidLinearApiUrl(const std::string& apiUrl);

/// Build the GraphQL request body string `{"query":"...","variables":{...}}`.
/// `variables` is embedded as-is (pass an empty object for variable-less
/// queries). Pure — no transport.
std::string BuildGraphQLBody(const std::string& query, const nlohmann::json& variables);

/// The Authorization header VALUE for a Linear personal API key. Linear personal
/// keys are sent raw, WITHOUT a `Bearer ` prefix (OAuth tokens use Bearer, but
/// OAuth is out of scope) — this one-liner pins that contract under test so a
/// future refactor can't silently re-add `Bearer `.
std::string BuildLinearAuthHeaderValue(const std::string& apiKey);

/// Inspect a parsed GraphQL response for a top-level `errors` array. Linear
/// returns errors there even on HTTP 200, so callers MUST check this before
/// reading `data`. Returns true + joins the error `message`s into `outMessage`
/// when `errors` is a non-empty array; false otherwise (`outMessage` cleared).
bool LinearResponseHasErrors(const nlohmann::json& parsed, std::string& outMessage);

/// Best-effort human-readable error message from a Linear GraphQL response body.
/// Prefers the joined `errors[].message`; falls back to `"HTTP <status>"` when
/// the body has no usable `errors` (or fails to parse). Used by write paths to
/// populate the audit-trail `errorMessage` and by sync warning classification.
std::string ExtractLinearErrorMessage(int httpStatus, const std::string& body);

/// Linear rate-limit snapshot parsed from the response headers. Linear sends
/// lowercase `x-ratelimit-requests-{limit,remaining,reset}`,
/// `x-ratelimit-complexity-{limit,remaining}` and `x-complexity` on every
/// response (see plan § Context). Absent/unparseable fields stay -1.
struct LinearRateLimit {
    long RequestsLimit = -1;
    long RequestsRemaining = -1;
    std::int64_t RequestsResetUnixSec = -1;
    long ComplexityLimit = -1;
    long ComplexityRemaining = -1;
    long Complexity = -1; // x-complexity: cost of THIS request

    /// True when at least one field parsed (i.e. the server sent rate headers).
    bool Present() const {
        return RequestsLimit >= 0 || RequestsRemaining >= 0 || RequestsResetUnixSec >= 0 ||
               ComplexityLimit >= 0 || ComplexityRemaining >= 0 || Complexity >= 0;
    }
};

/// Parse the Linear rate-limit headers. `headersLower` maps LOWERCASED header
/// names to values (the client lowercases cpr's header keys before calling, so
/// this stays cpr-free + unit-testable). Unknown/missing keys leave the
/// corresponding field at -1.
LinearRateLimit ParseLinearRateLimitHeaders(const std::map<std::string, std::string>& headersLower);

/// Per-request Linear credential resolution (issue #979 pattern, mirrors
/// `ResolveGitHubRequestAuth`): the live cfg API key is used unconditionally (an
/// empty live key means the user cleared the credential — no fallback to a
/// possibly-revoked ctor snapshot), while the API URL falls back to the ctor
/// snapshot and finally to `kDefaultLinearApiUrl`. Pure — unit-tested without
/// network.
struct LinearRequestAuth {
    std::string ApiUrl;
    std::string ApiKey;
};

LinearRequestAuth ResolveLinearRequestAuth(const std::string& cfgApiUrl, const std::string& cfgApiKey,
                                           const std::string& fallbackApiUrl);

} // namespace linear
} // namespace smatchet

#endif // SMATCHET_LINEAR_CLIENT_HELPERS_H
