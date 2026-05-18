// GitHubClientHelpers.h — pure helpers for the GitHub-backed ITrackerClient.
//
// All functions here are pure C++14 (no cpr / SQLite / ImGui / nlohmann::json).
// Doctest at tests/Source_Core/GitHubClientHelpers.test.cpp. This TU is
// source-list-conditional on SMATCHET_WITH_AGENTIC in the root CMakeLists.txt,
// so the no-agentic build skips compiling it entirely.
//
// Issue-key encoding rationale: GitHub identifies an issue by (owner, repo,
// number). Smatchet's existing `ITrackerClient` surface assumes a single
// stringly-typed key. The canonical Smatchet form chosen for cross-backend
// portability is `owner/repo#N` (e.g. `smatchet/example#42`). Round-trips are
// case-preserving on owner/repo per GitHub's API (owner / repo are
// case-insensitive at lookup time but case-preserving on display).

#ifndef SMATCHET_GITHUB_CLIENT_HELPERS_H
#define SMATCHET_GITHUB_CLIENT_HELPERS_H

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace GitHubClientHelpers {

struct ParsedIssueKey {
    std::string Owner;
    std::string Repo;
    std::int64_t Number = 0;
};

/**
 * Parse a Smatchet GitHub issue key (`owner/repo#N`).
 *
 * Rejects (returns false + populates outError):
 *   - empty input
 *   - missing `/` between owner and repo
 *   - missing `#` between repo and number
 *   - empty owner or empty repo
 *   - non-numeric N (anything other than 1+ ASCII digits)
 *   - leading whitespace / trailing characters after N
 *   - N <= 0 (GitHub issue numbers are >= 1)
 *
 * On success out is populated and outError is left empty.
 */
bool ParseGitHubIssueKey(const std::string& key, ParsedIssueKey& out, std::string& outError);

/**
 * Format an `owner/repo#N` key. Round-trips `Parse → Format → Parse`. Owner/repo
 * are copied verbatim (no case mutation); the caller is responsible for the
 * casing they want stored. Number must be > 0 (caller invariant); negative or
 * zero numbers format anyway so the bug surfaces at parse time, not silently.
 */
std::string FormatGitHubIssueKey(const std::string& owner, const std::string& repo, std::int64_t number);

/**
 * Parse a GitHub ISO-8601 timestamp (`YYYY-MM-DDTHH:MM:SSZ`) to unix epoch
 * seconds (UTC). GitHub's REST API always returns the Z-suffixed form; the
 * parser rejects fractional seconds, alternate offsets (`+02:00`), and any
 * shape other than the canonical 20-byte UTC ISO-8601 layout.
 *
 * Returns true on success and sets `outUnixSec`; on failure returns false and
 * populates `outError`. Caller policy: failures land as `0` in
 * `TrackerIssueComment::CreatedAtSec` and the parser logs once-latched at the
 * call site — comments do not get dropped because their timestamps failed to
 * parse (the body is still useful to the prompt builder).
 */
bool ParseIso8601ToUnixSec(const std::string& iso8601, std::int64_t& outUnixSec, std::string& outError);

/**
 * URL-suffix builders. Each returns the URL path appended to
 * `/repos/{owner}/{repo}/issues/{N}` for a given GitHub REST endpoint. Splitting
 * these off from the live `GitHubClient` makes the suffixes doctest-covered
 * without instantiating cpr. Format is exactly what GitHub's REST docs prescribe.
 *
 * Examples (for parsed.Owner="smatchet", parsed.Repo="example", parsed.Number=42):
 *   BuildIssueCommentsSuffix(parsed)  → "/repos/smatchet/example/issues/42/comments"
 *   BuildIssueLabelsSuffix(parsed)    → "/repos/smatchet/example/issues/42/labels"
 *   BuildIssueLabelRemoveSuffix(parsed, "bug")
 *                                     → "/repos/smatchet/example/issues/42/labels/bug"
 *   BuildIssueAssigneesSuffix(parsed) → "/repos/smatchet/example/issues/42/assignees"
 *   BuildIssueRootSuffix(parsed)      → "/repos/smatchet/example/issues/42"
 *
 * `BuildIssueLabelRemoveSuffix` URL-encodes the label name (RFC 3986 unreserved
 * set + percent-encoded everything else) so a label like `"prio: P0"` lands as
 * `prio%3A%20P0`. Other suffixes take no user-supplied path segment.
 */
struct ParsedIssueKey; // forward — defined above; redeclared for builders' signatures.

std::string BuildIssueCommentsSuffix(const ParsedIssueKey& parsed);
std::string BuildIssueLabelsSuffix(const ParsedIssueKey& parsed);
std::string BuildIssueLabelRemoveSuffix(const ParsedIssueKey& parsed, const std::string& labelName);
std::string BuildIssueAssigneesSuffix(const ParsedIssueKey& parsed);
std::string BuildIssueRootSuffix(const ParsedIssueKey& parsed);

/**
 * JSON body builders for the GitHub issues REST write methods. Each returns a
 * `nlohmann::json` shape exactly as GitHub's REST API expects. Pure, doctested.
 *
 *   BuildCommentAddBody("hello")            → {"body":"hello"}
 *   BuildLabelAddBody("bug")                → ["bug"]    (GitHub takes an array)
 *   BuildAssigneeSetBody("alice")           → {"assignees":["alice"]}
 *   BuildStateTransitionBody("closed")      → {"state":"closed"}
 */
nlohmann::json BuildCommentAddBody(const std::string& body);
nlohmann::json BuildLabelAddBody(const std::string& label);
nlohmann::json BuildAssigneeSetBody(const std::string& user);
nlohmann::json BuildStateTransitionBody(const std::string& state);

/**
 * State-transition validator. The agentic-flow contract locks state to exactly
 * `"open"` or `"closed"` (per `agentic-flow-implementation.md` § Decisions
 * locked #3, `StateTransition` payload shape). Any other input must be rejected
 * before any HTTP traffic is fired.
 */
bool IsValidStateTransitionTarget(const std::string& state);

/**
 * Extract GitHub's structured error `message` (`{"message": "...", ...}`) from
 * a 4xx/5xx response body. Returns the message string when present, empty
 * otherwise. Used to surface a one-line cause to the caller in addition to the
 * status-code summary. Body itself is run through the AI redactor at the call
 * site — `outMessage` is the same redacted-safe substring; no extra redaction.
 */
bool ExtractGitHubErrorMessage(const std::string& responseBody, std::string& outMessage);

/**
 * Percent-encode a single URL path segment (RFC 3986 unreserved). Used by
 * `BuildIssueLabelRemoveSuffix` since label names can contain `:`, ` `, `/`,
 * Unicode, etc. The set kept unencoded is `[A-Za-z0-9-._~]`; everything else
 * (including the `/` separator a hostile label might inject) is `%HH`-encoded.
 * Pure ASCII output.
 */
std::string PercentEncodePathSegment(const std::string& segment);

} // namespace GitHubClientHelpers

#endif // SMATCHET_GITHUB_CLIENT_HELPERS_H
