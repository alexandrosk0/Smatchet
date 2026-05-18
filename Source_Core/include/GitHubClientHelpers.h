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
 * Parse the `owner/repo` query form used by the scheduled-poll worker and the
 * `agent.triage.run --query` CLI surface. The shape is a strict subset of the
 * issue-key parser above: no `#N` suffix, exactly one `/`, both halves
 * non-empty, and no leading or trailing whitespace anywhere in the string.
 *
 * Rejects (returns false + populates outError):
 *   - empty input
 *   - missing `/` separator
 *   - more than one `/`
 *   - empty owner or empty repo segment
 *   - any leading or trailing ASCII whitespace (' ', '\t', '\r', '\n') in the
 *     overall string or either segment — strict so the CLI surfaces typos
 *     rather than silently lopping spaces off the user's input
 *
 * On success out is populated and outError is left empty.
 */
bool ParseGitHubRepoKey(const std::string& query, std::string& outOwner, std::string& outRepo, std::string& outError);

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
 * Format a unix epoch second (UTC) as the canonical GitHub ISO-8601 form
 * `YYYY-MM-DDTHH:MM:SSZ`. Inverse of `ParseIso8601ToUnixSec`. Used by the
 * scheduled-poll cursor — GitHub's `/issues?since=<iso8601>` filter expects
 * exactly this 20-char Z-suffixed UTC layout. Negative epochs are clamped to
 * 0 (pre-1970 has no meaning for an "updated since" cursor).
 *
 * Round-trip guarantee: `Parse(Format(t)) == t` for every t in
 * `[0, 253402300799]` (year 9999), provided `gmtime` is reentrant on the
 * platform (it is on every platform Smatchet targets).
 */
std::string FormatUnixSecAsIso8601(std::int64_t unixSec);

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

// ─── H7 PR-thread + workflow URL builders ────────────────────────────────────
//
// Each builder returns the URL path appended to the GitHub REST root for a
// specific endpoint. Splitting these off from `GitHubClient` makes them
// doctest-covered without instantiating cpr. The shapes match the GitHub REST
// API v2022-11-28 documentation linked from each method in `GitHubClient.h`.

/**
 * `/repos/{owner}/{repo}/pulls` — POST endpoint for creating a pull request.
 * Owner/repo are NOT URL-encoded (GitHub disallows special chars in either).
 */
std::string BuildPullsCollectionSuffix(const std::string& owner, const std::string& repo);

/**
 * `/repos/{owner}/{repo}/commits/{sha}/check-runs` — GET endpoint for listing
 * check runs against a commit SHA. The SHA is copied verbatim; the caller is
 * responsible for ensuring it's a hex SHA (validated upstream by the
 * watcher's git observer).
 */
std::string BuildCheckRunsForCommitSuffix(const std::string& owner, const std::string& repo, const std::string& sha);

/**
 * `/repos/{owner}/{repo}/check-runs/{id}/annotations` — GET endpoint for the
 * file-level annotations attached to one check run.
 */
std::string BuildCheckRunAnnotationsSuffix(const std::string& owner, const std::string& repo, std::int64_t checkRunId);

/**
 * `/repos/{owner}/{repo}/actions/jobs/{id}/logs` — GET endpoint for a workflow
 * job's raw log text. Returns 302 to a presigned blob; cpr follows.
 */
std::string BuildActionsJobLogsSuffix(const std::string& owner, const std::string& repo, std::int64_t jobId);

/**
 * `/repos/{owner}/{repo}/actions/runs/{id}/rerun` — POST endpoint to re-run
 * a failed workflow run.
 */
std::string BuildActionsRunRerunSuffix(const std::string& owner, const std::string& repo, std::int64_t runId);

/// JSON body for the `POST /pulls` create-pull-request call. Field shape
/// mirrors the GitHub REST docs verbatim:
///   {
///     "title":  "...",
///     "head":   "<headBranch>",
///     "base":   "<baseBranch>",
///     "body":   "...",      (omitted when empty)
///     "draft":  true|false
///   }
nlohmann::json BuildCreatePullRequestBody(const std::string& title, const std::string& headBranch,
                                          const std::string& baseBranch, const std::string& body, bool draft);

/// Extract the `html_url` field from a GitHub create-pull-request response.
/// Returns true + populates `outUrl` on the 201 happy path; returns false
/// when the field is missing, empty, or not a string. The body itself may
/// carry many other fields the caller doesn't need (id, node_id, state,
/// etc) — we extract only the URL.
bool ExtractCreatePullRequestHtmlUrl(const std::string& responseBody, std::string& outUrl, std::string& outError);

/// Clip an arbitrarily long log body to the last `tailLines` lines (split on
/// '\n'). `tailLines <= 0` returns the body verbatim. Pure helper — does not
/// allocate when the input is already within the bound.
std::string ClipLogTail(const std::string& body, int tailLines);

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

/**
 * Validate the GitHub REST base URL used by `GitHubClient`. GitHub Enterprise
 * deployments pass non-`api.github.com` hosts (e.g. `https://github.acme.com/api/v3`).
 * Validation rules:
 *   - non-empty
 *   - exact `https://` scheme (case-sensitive — GitHub never serves over `http://`,
 *     and dangerous schemes `javascript:`, `file:`, `data:`, `gopher:` etc. are
 *     rejected explicitly so a hostile config write can't redirect traffic to a
 *     non-HTTP destination)
 *   - non-empty authority after the scheme (so `https://` alone fails)
 *
 * Returns true on a usable base URL. On failure populates `outError` with a
 * one-line human-readable reason.
 */
bool IsValidGitHubBaseUrl(const std::string& baseUrl, std::string& outError);

/**
 * Hard cap on the JSON body sent in any outbound `cpr::Post`/`cpr::Patch` to the
 * GitHub REST API. GitHub's own per-endpoint payload limits sit in the
 * 1-65 KiB range (comment body: 65 KiB; label/assignee bodies are much smaller).
 * The agentic flow rejects bodies above this cap pre-HTTP so a hostile or
 * malformed LLM proposal cannot cause an unbounded request — both as a Pillar 3
 * defense and to keep the network round-trip predictable.
 */
constexpr std::size_t kMaxGitHubRequestBodyBytes = 256 * 1024;

/**
 * Pure predicate — true iff `bodyByteLength` exceeds the outbound request body
 * cap. Lives in the pure-helper TU so the doctest rig can exercise the cap
 * without instantiating cpr. Used by `GitHubClient` write methods to reject
 * over-large payloads before any HTTP traffic is fired.
 */
bool ShouldRejectAsTooLarge(std::size_t bodyByteLength);

} // namespace GitHubClientHelpers

#endif // SMATCHET_GITHUB_CLIENT_HELPERS_H
