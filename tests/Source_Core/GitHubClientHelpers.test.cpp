#include <doctest/doctest.h>

#include "GitHubClientHelpers.h"

#include <cstdint>
#include <string>

using GitHubClientHelpers::FormatGitHubIssueKey;
using GitHubClientHelpers::ParseGitHubIssueKey;
using GitHubClientHelpers::ParseIso8601ToUnixSec;
using GitHubClientHelpers::ParsedIssueKey;

TEST_CASE("ParseGitHubIssueKey — happy path") {
    SUBCASE("canonical owner/repo#N") {
        ParsedIssueKey out;
        std::string err;
        CHECK(ParseGitHubIssueKey("smatchet/example#42", out, err));
        CHECK(err.empty());
        CHECK(out.Owner == "smatchet");
        CHECK(out.Repo == "example");
        CHECK(out.Number == 42);
    }
    SUBCASE("multi-digit issue number") {
        ParsedIssueKey out;
        std::string err;
        CHECK(ParseGitHubIssueKey("alexandrosk0/Smatchet#1234567", out, err));
        CHECK(out.Number == 1234567);
    }
    SUBCASE("owner / repo with hyphens, underscores, digits") {
        ParsedIssueKey out;
        std::string err;
        CHECK(ParseGitHubIssueKey("acme-corp/my_repo-v2#7", out, err));
        CHECK(out.Owner == "acme-corp");
        CHECK(out.Repo == "my_repo-v2");
        CHECK(out.Number == 7);
    }
    SUBCASE("issue number = 1 (lower bound)") {
        ParsedIssueKey out;
        std::string err;
        CHECK(ParseGitHubIssueKey("a/b#1", out, err));
        CHECK(out.Number == 1);
    }
}

TEST_CASE("ParseGitHubIssueKey — error cases") {
    ParsedIssueKey out;
    std::string err;
    SUBCASE("empty input") {
        CHECK_FALSE(ParseGitHubIssueKey("", out, err));
        CHECK(!err.empty());
    }
    SUBCASE("missing slash") {
        CHECK_FALSE(ParseGitHubIssueKey("owner-repo#42", out, err));
        CHECK(err.find("'/'") != std::string::npos);
    }
    SUBCASE("missing hash") {
        CHECK_FALSE(ParseGitHubIssueKey("owner/repo-42", out, err));
        CHECK(err.find("'#'") != std::string::npos);
    }
    SUBCASE("empty owner") {
        CHECK_FALSE(ParseGitHubIssueKey("/repo#42", out, err));
        CHECK(err.find("empty owner") != std::string::npos);
    }
    SUBCASE("empty repo") {
        CHECK_FALSE(ParseGitHubIssueKey("owner/#42", out, err));
        CHECK(err.find("empty repo") != std::string::npos);
    }
    SUBCASE("empty number") {
        CHECK_FALSE(ParseGitHubIssueKey("owner/repo#", out, err));
        CHECK(err.find("empty issue number") != std::string::npos);
    }
    SUBCASE("non-numeric N") {
        CHECK_FALSE(ParseGitHubIssueKey("owner/repo#abc", out, err));
        CHECK(err.find("non-digit") != std::string::npos);
    }
    SUBCASE("N with trailing letters") {
        CHECK_FALSE(ParseGitHubIssueKey("owner/repo#42abc", out, err));
        CHECK(err.find("non-digit") != std::string::npos);
    }
    SUBCASE("N is zero") {
        CHECK_FALSE(ParseGitHubIssueKey("owner/repo#0", out, err));
        CHECK(err.find(">= 1") != std::string::npos);
    }
    SUBCASE("leading whitespace") {
        CHECK_FALSE(ParseGitHubIssueKey("  owner/repo#42", out, err));
        CHECK(err.find("whitespace") != std::string::npos);
    }
}

TEST_CASE("FormatGitHubIssueKey — basic") {
    CHECK(FormatGitHubIssueKey("smatchet", "example", 42) == "smatchet/example#42");
    CHECK(FormatGitHubIssueKey("a", "b", 1) == "a/b#1");
    CHECK(FormatGitHubIssueKey("acme-corp", "my_repo-v2", 1234567) == "acme-corp/my_repo-v2#1234567");
}

TEST_CASE("Parse → Format → Parse round-trip identity") {
    const std::string keys[] = {
        "smatchet/example#42",
        "a/b#1",
        "acme-corp/my_repo-v2#7",
        "alexandrosk0/Smatchet#1234567",
    };
    for (const std::string& k : keys) {
        ParsedIssueKey p;
        std::string err;
        CHECK(ParseGitHubIssueKey(k, p, err));
        const std::string formatted = FormatGitHubIssueKey(p.Owner, p.Repo, p.Number);
        CHECK(formatted == k);
        ParsedIssueKey p2;
        std::string err2;
        CHECK(ParseGitHubIssueKey(formatted, p2, err2));
        CHECK(p2.Owner == p.Owner);
        CHECK(p2.Repo == p.Repo);
        CHECK(p2.Number == p.Number);
    }
}

TEST_CASE("ParseIso8601ToUnixSec — happy path") {
    SUBCASE("unix epoch — 1970-01-01T00:00:00Z") {
        std::int64_t ts = -1;
        std::string err;
        CHECK(ParseIso8601ToUnixSec("1970-01-01T00:00:00Z", ts, err));
        CHECK(ts == 0);
    }
    SUBCASE("GitHub-shape canonical timestamp") {
        std::int64_t ts = 0;
        std::string err;
        // 2024-01-15T12:34:56Z is 1705321xxx range — verify via known unix-epoch.
        CHECK(ParseIso8601ToUnixSec("2024-01-15T12:34:56Z", ts, err));
        // 2024-01-15 12:34:56 UTC = 1705322096
        CHECK(ts == 1705322096);
    }
    SUBCASE("year 2000 boundary") {
        std::int64_t ts = 0;
        std::string err;
        CHECK(ParseIso8601ToUnixSec("2000-01-01T00:00:00Z", ts, err));
        CHECK(ts == 946684800);
    }
}

TEST_CASE("ParseIso8601ToUnixSec — error cases") {
    std::int64_t ts = 0;
    std::string err;
    SUBCASE("wrong length") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15T12:34:56", ts, err));
        CHECK_FALSE(ParseIso8601ToUnixSec("", ts, err));
    }
    SUBCASE("non-Z suffix (timezone offset)") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15T12:34:56+", ts, err));
    }
    SUBCASE("fractional seconds (rejected — wrong length)") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15T12:34:56.789Z", ts, err));
    }
    SUBCASE("malformed separators") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024/01/15T12:34:56Z", ts, err));
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15 12:34:56Z", ts, err));
    }
    SUBCASE("non-digit in numeric field") {
        CHECK_FALSE(ParseIso8601ToUnixSec("abcd-01-15T12:34:56Z", ts, err));
    }
    SUBCASE("month out of range") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-13-15T12:34:56Z", ts, err));
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-00-15T12:34:56Z", ts, err));
    }
    SUBCASE("day out of range") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-32T12:34:56Z", ts, err));
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-00T12:34:56Z", ts, err));
    }
    SUBCASE("hour/minute/second out of range") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15T24:34:56Z", ts, err));
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15T12:60:56Z", ts, err));
        // Leap-second allowed (sec == 60 is RFC3339-permitted).
        std::int64_t lts = 0;
        std::string lerr;
        CHECK(ParseIso8601ToUnixSec("2024-01-15T12:34:60Z", lts, lerr));
    }
}
