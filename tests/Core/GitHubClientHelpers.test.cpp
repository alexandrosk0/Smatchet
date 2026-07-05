// GitHubClientHelpers doctest — pure helper round-trips.
// Part of docs/plans/shipped/github-tracker-backend.md.

#include "GitHubClientHelpers.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

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
    // Accept the two canonical shapes — Ok(true), no error.
    CHECK(IsValidGitHubBaseUrl("https://api.github.com"));
    CHECK(IsValidGitHubBaseUrl("https://api.github.com").value());
    CHECK(IsValidGitHubBaseUrl("https://github.example.com/api/v3"));
    // Reject empty / non-https / trailing-slash / random path — Err carries the message.
    const auto empty = IsValidGitHubBaseUrl("");
    CHECK_FALSE(empty);
    CHECK_FALSE(empty.error().empty());
    const auto notHttps = IsValidGitHubBaseUrl("http://api.github.com");
    CHECK_FALSE(notHttps);
    CHECK(notHttps.error().find("https") != std::string::npos);
    const auto trailing = IsValidGitHubBaseUrl("https://api.github.com/");
    CHECK_FALSE(trailing);
    CHECK(trailing.error().find("trailing slash") != std::string::npos);
    // Per CodeRabbit review — reject arbitrary https paths.
    CHECK_FALSE(IsValidGitHubBaseUrl("https://github.com")); // not /api/v3
    CHECK_FALSE(IsValidGitHubBaseUrl("https://example.com/somewhere"));
    CHECK_FALSE(IsValidGitHubBaseUrl("https:///api/v3")); // empty host
}

// github-commit-tracker-rows — commit key parser + commit URL helpers.

TEST_CASE("ParseGitHubCommitKey — canonical owner/repo@<full-sha>") {
    ParsedCommitKey k;
    REQUIRE(ParseGitHubCommitKey("alexandrosk0/Smatchet@a1b2c3d4e5f60718293a4b5c6d7e8f9012345678", k));
    CHECK(k.Owner == "alexandrosk0");
    CHECK(k.Repo == "Smatchet");
    CHECK(k.Sha == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
}

TEST_CASE("ParseGitHubCommitKey — accepts abbreviated 7-char sha + hyphen/dot repo") {
    ParsedCommitKey k;
    REQUIRE(ParseGitHubCommitKey("foo/my-repo.git@a1b2c3d", k));
    CHECK(k.Owner == "foo");
    CHECK(k.Repo == "my-repo.git");
    CHECK(k.Sha == "a1b2c3d");
}

TEST_CASE("ParseGitHubCommitKey — rejects malformed keys") {
    ParsedCommitKey k;
    CHECK_FALSE(ParseGitHubCommitKey("", k));
    CHECK_FALSE(ParseGitHubCommitKey("noAt/repo", k));          // no @
    CHECK_FALSE(ParseGitHubCommitKey("owner/repo@", k));        // empty sha
    CHECK_FALSE(ParseGitHubCommitKey("/repo@a1b2c3d", k));      // empty owner
    CHECK_FALSE(ParseGitHubCommitKey("owner/@a1b2c3d", k));     // empty repo
    CHECK_FALSE(ParseGitHubCommitKey("owner/repo@ghijklm", k)); // non-hex sha
    CHECK_FALSE(ParseGitHubCommitKey("owner/repo@abc12", k));   // sha too short (<7)
    CHECK_FALSE(ParseGitHubCommitKey("owner/repo#42", k));      // issue key, not commit
    // sha too long (>40)
    CHECK_FALSE(ParseGitHubCommitKey("owner/repo@a1b2c3d4e5f60718293a4b5c6d7e8f90123456789", k));
}

TEST_CASE("BuildCommitListUrlSuffix — clamps perPage") {
    CHECK(BuildCommitListUrlSuffix("alexandrosk0", "Smatchet", 100) ==
          "/repos/alexandrosk0/Smatchet/commits?per_page=100");
    CHECK(BuildCommitListUrlSuffix("o", "r", 0) == "/repos/o/r/commits?per_page=1");
    CHECK(BuildCommitListUrlSuffix("o", "r", 9999) == "/repos/o/r/commits?per_page=100");
}

TEST_CASE("BuildCommitBrowseUrlSuffix — shapes /owner/repo/commit/sha") {
    CHECK(BuildCommitBrowseUrlSuffix("alexandrosk0", "Smatchet", "a1b2c3d") == "/alexandrosk0/Smatchet/commit/a1b2c3d");
}

TEST_CASE("ExtractGitHubErrorMessage — pulls 'message' from JSON; falls back to HTTP code") {
    CHECK(ExtractGitHubErrorMessage(404, R"({"message":"Not Found","documentation_url":"..."})") == "Not Found");
    CHECK(ExtractGitHubErrorMessage(500, "") == "HTTP 500");
    CHECK(ExtractGitHubErrorMessage(403, "not json at all") == "HTTP 403");
}

TEST_CASE("ParseIso8601ToUnixSec — timezone-aware (Z + ±HH:MM + ±HHMM)") {
    // Ok path — value carries the epoch seconds.
    {
        const auto r = ParseIso8601ToUnixSec("1970-01-01T00:00:00Z");
        REQUIRE(r);
        CHECK(r.value() == 0);
    }
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00Z");
        REQUIRE(r);
        CHECK(r.value() == 1705276800);
    }
    // +00:00 equals Z.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00+00:00");
        REQUIRE(r);
        CHECK(r.value() == 1705276800);
    }
    // Per CodeRabbit review — non-zero offset now adjusts the epoch.
    // 2024-01-15T12:00:00 +05:30 = 2024-01-15T06:30:00 UTC.
    // 1705276800 (Jan 15 00:00 UTC) + 12*3600 (12h) - 19800 (5h30m offset) = 1705300200.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T12:00:00+05:30");
        REQUIRE(r);
        CHECK(r.value() == 1705300200);
    }
    // Negative offset.
    // 2024-01-15T00:00:00 -08:00 = 2024-01-15T08:00:00 UTC = 1705305600.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00-08:00");
        REQUIRE(r);
        CHECK(r.value() == 1705305600);
    }
    // No-colon offset form.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00+0530");
        REQUIRE(r);
        CHECK(r.value() == 1705257000);
    }
    // Missing suffix is now an error (was silently treated as UTC).
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00");
        CHECK_FALSE(r);
        CHECK(r.error().find("timezone") != std::string::npos);
    }
    // Bad format.
    {
        const auto r = ParseIso8601ToUnixSec("not a date");
        CHECK_FALSE(r);
        CHECK_FALSE(r.error().empty());
    }
    // Unrecognised suffix.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00X");
        CHECK_FALSE(r);
        CHECK_FALSE(r.error().empty());
    }
    // Per CodeRabbit nitpick — out-of-range offsets rejected (max real-world is +14:00).
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00+53:99");
        CHECK_FALSE(r);
        CHECK(r.error().find("out of range") != std::string::npos);
    }
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00+15:00");
        CHECK_FALSE(r);
        CHECK(r.error().find("out of range") != std::string::npos);
    }
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00+14:01");
        CHECK_FALSE(r);
        CHECK(r.error().find("out of range") != std::string::npos);
    }
    // issue-comments PR-B — Jira stamps millisecond precision ("...:00.000+0000").
    // The fractional-second component is stripped before the timezone is classified,
    // so a ms timestamp resolves to the same epoch as its second-precision form.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00.000+0000");
        REQUIRE(r);
        CHECK(r.value() == 1705276800);
    }
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00.123Z");
        REQUIRE(r);
        CHECK(r.value() == 1705276800);
    }
    // Fractional seconds coexist with a non-zero colon offset.
    // 2024-01-15T00:00:00 +05:30 = 2024-01-14T18:30:00 UTC = 1705257000.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00.500+05:30");
        REQUIRE(r);
        CHECK(r.value() == 1705257000);
    }
    // A bare '.' with no fractional digit must NOT be silently accepted by stripping the dot
    // and reading the remainder as the timezone suffix.
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00.Z");
        CHECK_FALSE(r);
    }
    {
        const auto r = ParseIso8601ToUnixSec("2024-01-15T00:00:00.+0000");
        CHECK_FALSE(r);
    }
}

// ---------------------------------------------------------------------------
// log-a-bug-github Slice 1 — issue-create payload builder + key formatter.
// ---------------------------------------------------------------------------

TEST_CASE("BuildGitHubCreatePayload — title/body/labels/assignees + __target") {
    const auto r =
        BuildGitHubCreatePayload("Grid freezes when sorting by date", "Steps:\n1. open grid\n2. sort by date",
                                 "bug, ui", "alice , bob", "acme", "tracker");
    REQUIRE(r);
    const nlohmann::json& out = r.value();
    CHECK(out["title"] == "Grid freezes when sorting by date");
    CHECK(out["body"] == "Steps:\n1. open grid\n2. sort by date");
    REQUIRE(out["labels"].is_array());
    CHECK(out["labels"].size() == 2);
    CHECK(out["labels"][0] == "bug");
    CHECK(out["labels"][1] == "ui");
    REQUIRE(out["assignees"].is_array());
    CHECK(out["assignees"].size() == 2);
    CHECK(out["assignees"][0] == "alice"); // surrounding whitespace trimmed
    CHECK(out["assignees"][1] == "bob");
    REQUIRE(out.contains("__target"));
    CHECK(out["__target"]["owner"] == "acme");
    CHECK(out["__target"]["repo"] == "tracker");
}

TEST_CASE("BuildGitHubCreatePayload — minimal (title only, no body/labels/assignees)") {
    const auto r = BuildGitHubCreatePayload("just a title", "", "", "", "o", "r");
    REQUIRE(r);
    const nlohmann::json& out = r.value();
    CHECK(out["title"] == "just a title");
    CHECK_FALSE(out.contains("body"));
    CHECK_FALSE(out.contains("labels"));
    CHECK_FALSE(out.contains("assignees"));
    CHECK(out["__target"]["owner"] == "o");
}

TEST_CASE("BuildGitHubCreatePayload — blank CSV entries are skipped, not emitted") {
    const auto r = BuildGitHubCreatePayload("t", "", " , ,, ", "", "o", "r");
    REQUIRE(r);
    // All-blank labels CSV → key omitted entirely.
    CHECK_FALSE(r.value().contains("labels"));
}

// ---------------------------------------------------------------------------
// Issue #979 — per-request credential resolution (PAT rotation without a fresh
// client). The client used to latch its ctor PAT for the whole session; a
// client constructed before the user entered a PAT stayed dead until restart.
// ---------------------------------------------------------------------------

TEST_CASE("ResolveGitHubRequestAuth — rotation: empty ctor snapshot, live cfg PAT wins" *
          doctest::test_suite("[high-risk]")) {
    // The #979 scenario: client constructed with empty creds (stale disk read),
    // then the user saves a PAT — the very next request must carry it.
    const GitHubRequestAuth auth = ResolveGitHubRequestAuth("https://api.github.com", "new-pat", "");
    CHECK(auth.Pat == "new-pat");
    CHECK(auth.BaseUrl == "https://api.github.com");
}

TEST_CASE("ResolveGitHubRequestAuth — live cfg PAT supersedes a different ctor snapshot") {
    const GitHubRequestAuth auth = ResolveGitHubRequestAuth("", "rotated-pat", "https://ghe.example/api/v3");
    CHECK(auth.Pat == "rotated-pat");
    // cfg base URL empty → ctor snapshot base URL retained.
    CHECK(auth.BaseUrl == "https://ghe.example/api/v3");
}

TEST_CASE("ResolveGitHubRequestAuth — cleared cfg PAT stays cleared (no ctor-snapshot fallback)" *
          doctest::test_suite("[high-risk]")) {
    // Review 2026-06-07: an empty live PAT means the user deliberately cleared the
    // credential — the client must NOT keep sending a possibly-revoked ctor snapshot.
    // Only the base URL falls back.
    const GitHubRequestAuth auth = ResolveGitHubRequestAuth("", "", "https://ghe.example/api/v3");
    CHECK(auth.Pat.empty());
    CHECK(auth.BaseUrl == "https://ghe.example/api/v3");
}

TEST_CASE("ResolveGitHubRequestAuth — everything empty: cloud default URL, empty PAT") {
    const GitHubRequestAuth auth = ResolveGitHubRequestAuth("", "", "");
    CHECK(auth.Pat.empty());
    CHECK(auth.BaseUrl == "https://api.github.com");
}

TEST_CASE("ResolveGitHubRequestAuth — cfg base URL wins over ctor snapshot") {
    const GitHubRequestAuth auth =
        ResolveGitHubRequestAuth("https://ghe2.example/api/v3", "p", "https://ghe1.example/api/v3");
    CHECK(auth.BaseUrl == "https://ghe2.example/api/v3");
}

TEST_CASE("BuildGitHubCreatePayload — rejects empty summary + empty repo target") {
    const auto emptySummary = BuildGitHubCreatePayload("", "body", "", "", "o", "r");
    CHECK_FALSE(emptySummary);
    CHECK_FALSE(emptySummary.error().empty());
    const auto emptyOwner = BuildGitHubCreatePayload("title", "", "", "", "", "r");
    CHECK_FALSE(emptyOwner);
    CHECK_FALSE(emptyOwner.error().empty());
    const auto emptyRepo = BuildGitHubCreatePayload("title", "", "", "", "o", "");
    CHECK_FALSE(emptyRepo);
    CHECK_FALSE(emptyRepo.error().empty());
}

TEST_CASE("FormatGitHubIssueKey — composes owner/repo#N (inverse of ParseGitHubIssueKey)") {
    CHECK(FormatGitHubIssueKey("acme", "tracker", 5) == "acme/tracker#5");
    // Round-trips through the parser.
    ParsedIssueKey k;
    REQUIRE(ParseGitHubIssueKey(FormatGitHubIssueKey("foo", "bar-baz", 123), k));
    CHECK(k.Owner == "foo");
    CHECK(k.Repo == "bar-baz");
    CHECK(k.Number == 123);
}
