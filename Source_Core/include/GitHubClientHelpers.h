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

} // namespace GitHubClientHelpers

#endif // SMATCHET_GITHUB_CLIENT_HELPERS_H
