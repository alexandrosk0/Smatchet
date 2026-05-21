// GitHubClientHelpers doctest — pure helper round-trips.
// PR2 of docs/design/github-tracker-backend.md.

#include "GitHubClientHelpers.h"

#include <doctest/doctest.h>

using namespace smatchet::github;

TEST_CASE("ParseGitHubIssueKey — canonical owner/repo#N") {
    ParsedIssueKey k;
    REQUIRE(ParseGitHubIssueKey("alexandrosk0/Smatchet#42", k));
    CHECK(k.Owner == "alexandrosk0");
    CHECK(k.Repo == "Smatchet");
    CHECK(k.Number == 42);
}

TEST_CASE("ParseGitHubIssueKey — large numbers + repo containing hyphen + dot") {
    ParsedIssueKey k;
    REQUIRE(ParseGitHubIssueKey("foo/my-repo.git#999999", k));
    CHECK(k.Owner == "foo");
    CHECK(k.Repo == "my-repo.git");
    CHECK(k.Number == 999999);
}

TEST_CASE("ParseGitHubIssueKey — rejects empty / missing parts / bad chars") {
    ParsedIssueKey k;
    CHECK_FALSE(ParseGitHubIssueKey("", k));
    CHECK_FALSE(ParseGitHubIssueKey("noSlash#1", k));
    CHECK_FALSE(ParseGitHubIssueKey("owner/repo", k));     // no #
    CHECK_FALSE(ParseGitHubIssueKey("owner/repo#", k));    // empty num
    CHECK_FALSE(ParseGitHubIssueKey("/repo#1", k));        // empty owner
    CHECK_FALSE(ParseGitHubIssueKey("owner/#1", k));       // empty repo
    CHECK_FALSE(ParseGitHubIssueKey("owner/repo#abc", k)); // non-numeric
    CHECK_FALSE(ParseGitHubIssueKey("owner/repo#0", k));   // non-positive
    CHECK_FALSE(ParseGitHubIssueKey("owner/repo#-1", k));  // negative
    CHECK_FALSE(ParseGitHubIssueKey("ow ner/repo#1", k));  // space
}

TEST_CASE("BuildIssueListUrlSuffix — clamps perPage + appends since when non-zero") {
    CHECK(BuildIssueListUrlSuffix("alexandrosk0", "Smatchet", 30, 0) ==
          "/repos/alexandrosk0/Smatchet/issues?per_page=30&state=all");
    CHECK(BuildIssueListUrlSuffix("o", "r", 0, 0) == "/repos/o/r/issues?per_page=1&state=all");
    CHECK(BuildIssueListUrlSuffix("o", "r", 9999, 0) == "/repos/o/r/issues?per_page=100&state=all");
    // 2024-01-15 00:00:00 UTC = 1705276800
    const std::string s = BuildIssueListUrlSuffix("o", "r", 10, 1705276800);
    CHECK(s.find("&since=2024-01-15T00:00:00Z") != std::string::npos);
}

TEST_CASE("BuildIssuePatchUrlSuffix — shapes /repos/o/r/issues/N") {
    CHECK(BuildIssuePatchUrlSuffix("alexandrosk0", "Smatchet", 42) == "/repos/alexandrosk0/Smatchet/issues/42");
    CHECK(BuildIssuePatchUrlSuffix("a", "b", 1) == "/repos/a/b/issues/1");
}

TEST_CASE("IsValidGitHubBaseUrl — strict: only api.github.com or <host>/api/v3 accepted") {
    std::string err;
    // Accept the two canonical shapes.
    CHECK(IsValidGitHubBaseUrl("https://api.github.com", err));
    CHECK(err.empty());
    CHECK(IsValidGitHubBaseUrl("https://github.example.com/api/v3", err));
    CHECK(err.empty());
    // Reject empty / non-https / trailing-slash / random path.
    CHECK_FALSE(IsValidGitHubBaseUrl("", err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(IsValidGitHubBaseUrl("http://api.github.com", err));
    CHECK(err.find("https") != std::string::npos);
    CHECK_FALSE(IsValidGitHubBaseUrl("https://api.github.com/", err));
    CHECK(err.find("trailing slash") != std::string::npos);
    // Per CodeRabbit on PR #357 — reject arbitrary https paths.
    CHECK_FALSE(IsValidGitHubBaseUrl("https://github.com", err)); // not /api/v3
    CHECK_FALSE(IsValidGitHubBaseUrl("https://example.com/somewhere", err));
    CHECK_FALSE(IsValidGitHubBaseUrl("https:///api/v3", err)); // empty host
}

TEST_CASE("ExtractGitHubErrorMessage — pulls 'message' from JSON; falls back to HTTP code") {
    CHECK(ExtractGitHubErrorMessage(404, R"({"message":"Not Found","documentation_url":"..."})") == "Not Found");
    CHECK(ExtractGitHubErrorMessage(500, "") == "HTTP 500");
    CHECK(ExtractGitHubErrorMessage(403, "not json at all") == "HTTP 403");
}

TEST_CASE("ParseIso8601ToUnixSec — timezone-aware (Z + ±HH:MM + ±HHMM)") {
    std::string err;
    CHECK(ParseIso8601ToUnixSec("1970-01-01T00:00:00Z", err) == 0);
    CHECK(err.empty());
    CHECK(ParseIso8601ToUnixSec("2024-01-15T00:00:00Z", err) == 1705276800);
    CHECK(err.empty());
    // +00:00 equals Z.
    CHECK(ParseIso8601ToUnixSec("2024-01-15T00:00:00+00:00", err) == 1705276800);
    CHECK(err.empty());
    // Per CodeRabbit on PR #357 — non-zero offset now adjusts the epoch.
    // 2024-01-15T12:00:00 +05:30 = 2024-01-15T06:30:00 UTC.
    // 1705276800 (Jan 15 00:00 UTC) + 12*3600 (12h) - 19800 (5h30m offset) = 1705300200.
    CHECK(ParseIso8601ToUnixSec("2024-01-15T12:00:00+05:30", err) == 1705300200);
    CHECK(err.empty());
    // Negative offset.
    // 2024-01-15T00:00:00 -08:00 = 2024-01-15T08:00:00 UTC = 1705305600.
    CHECK(ParseIso8601ToUnixSec("2024-01-15T00:00:00-08:00", err) == 1705305600);
    CHECK(err.empty());
    // No-colon offset form.
    CHECK(ParseIso8601ToUnixSec("2024-01-15T00:00:00+0530", err) == 1705257000);
    CHECK(err.empty());
    // Missing suffix is now an error (was silently treated as UTC).
    CHECK(ParseIso8601ToUnixSec("2024-01-15T00:00:00", err) == 0);
    CHECK(err.find("timezone") != std::string::npos);
    // Bad format.
    CHECK(ParseIso8601ToUnixSec("not a date", err) == 0);
    CHECK_FALSE(err.empty());
    // Unrecognised suffix.
    CHECK(ParseIso8601ToUnixSec("2024-01-15T00:00:00X", err) == 0);
    CHECK_FALSE(err.empty());
}
