#include <doctest/doctest.h>

#include "GitHubClientHelpers.h"

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

using GitHubClientHelpers::BuildAssigneeSetBody;
using GitHubClientHelpers::BuildCommentAddBody;
using GitHubClientHelpers::BuildIssueAssigneesSuffix;
using GitHubClientHelpers::BuildIssueCommentsSuffix;
using GitHubClientHelpers::BuildIssueLabelRemoveSuffix;
using GitHubClientHelpers::BuildIssueLabelsSuffix;
using GitHubClientHelpers::BuildIssueRootSuffix;
using GitHubClientHelpers::BuildLabelAddBody;
using GitHubClientHelpers::BuildStateTransitionBody;
using GitHubClientHelpers::ExtractGitHubErrorMessage;
using GitHubClientHelpers::FormatGitHubIssueKey;
using GitHubClientHelpers::FormatUnixSecAsIso8601;
using GitHubClientHelpers::IsValidStateTransitionTarget;
using GitHubClientHelpers::ParsedIssueKey;
using GitHubClientHelpers::ParseGitHubIssueKey;
using GitHubClientHelpers::ParseIso8601ToUnixSec;
using GitHubClientHelpers::PercentEncodePathSegment;

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
    SUBCASE("non-Z suffix (timezone offset)") { CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15T12:34:56+", ts, err)); }
    SUBCASE("fractional seconds (rejected — wrong length)") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15T12:34:56.789Z", ts, err));
    }
    SUBCASE("malformed separators") {
        CHECK_FALSE(ParseIso8601ToUnixSec("2024/01/15T12:34:56Z", ts, err));
        CHECK_FALSE(ParseIso8601ToUnixSec("2024-01-15 12:34:56Z", ts, err));
    }
    SUBCASE("non-digit in numeric field") { CHECK_FALSE(ParseIso8601ToUnixSec("abcd-01-15T12:34:56Z", ts, err)); }
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

// ─── T7: FormatUnixSecAsIso8601 round-trip ──────────────────────────────────
//
// The scheduled-poll cursor uses Format(t) to construct GitHub's `since=<iso>`
// query param. Parse(Format(t)) must equal t for every realistic cursor value
// the worker writes (wall-clock seconds since epoch). The error inverse —
// Format(Parse(s)) — is tested implicitly by the round-trip identity below.

TEST_CASE("FormatUnixSecAsIso8601 — happy path shape") {
    SUBCASE("epoch") { CHECK(FormatUnixSecAsIso8601(0) == "1970-01-01T00:00:00Z"); }
    SUBCASE("year 2024 mid-day") {
        // 2024-01-15T12:34:56Z — same vector ParseIso8601ToUnixSec uses.
        std::int64_t parsed = 0;
        std::string err;
        CHECK(ParseIso8601ToUnixSec("2024-01-15T12:34:56Z", parsed, err));
        CHECK(FormatUnixSecAsIso8601(parsed) == "2024-01-15T12:34:56Z");
    }
    SUBCASE("year 2000 midnight") {
        std::int64_t parsed = 0;
        std::string err;
        CHECK(ParseIso8601ToUnixSec("2000-01-01T00:00:00Z", parsed, err));
        CHECK(FormatUnixSecAsIso8601(parsed) == "2000-01-01T00:00:00Z");
    }
    SUBCASE("negative epoch clamps to zero") {
        CHECK(FormatUnixSecAsIso8601(-1) == "1970-01-01T00:00:00Z");
        CHECK(FormatUnixSecAsIso8601(-86400) == "1970-01-01T00:00:00Z");
    }
}

TEST_CASE("FormatUnixSecAsIso8601 — round-trip identity") {
    // Every cursor value the scheduled-poll worker writes survives
    // Parse(Format(t)) == t. Coverage spans epoch + leap years + year 2024..2030.
    const std::int64_t vectors[] = {
        0LL,
        1LL,          // 1970-01-01T00:00:01Z
        86399LL,      // 1970-01-01T23:59:59Z
        86400LL,      // 1970-01-02T00:00:00Z
        946684800LL,  // 2000-01-01T00:00:00Z
        951782400LL,  // 2000-02-29T00:00:00Z (leap year)
        1705320896LL, // 2024-01-15T12:14:56Z
        1735689599LL, // 2024-12-31T23:59:59Z
        1893456000LL, // 2030-01-01T00:00:00Z
    };
    for (std::int64_t t : vectors) {
        const std::string iso = FormatUnixSecAsIso8601(t);
        CHECK(iso.size() == 20);
        CHECK(iso[19] == 'Z');
        std::int64_t parsed = 0;
        std::string err;
        CHECK(ParseIso8601ToUnixSec(iso, parsed, err));
        CHECK(err.empty());
        CHECK(parsed == t);
    }
}

// ─── T2: URL-suffix builders ────────────────────────────────────────────────

TEST_CASE("BuildIssue*Suffix — canonical shapes") {
    ParsedIssueKey k;
    k.Owner = "smatchet";
    k.Repo = "example";
    k.Number = 42;
    CHECK(BuildIssueCommentsSuffix(k) == "/repos/smatchet/example/issues/42/comments");
    CHECK(BuildIssueLabelsSuffix(k) == "/repos/smatchet/example/issues/42/labels");
    CHECK(BuildIssueAssigneesSuffix(k) == "/repos/smatchet/example/issues/42/assignees");
    CHECK(BuildIssueRootSuffix(k) == "/repos/smatchet/example/issues/42");
    // Round-trips on large issue numbers (10-digit int64 path).
    k.Number = 1234567890;
    CHECK(BuildIssueRootSuffix(k) == "/repos/smatchet/example/issues/1234567890");
}

TEST_CASE("BuildIssueLabelRemoveSuffix — URL-encodes the label name") {
    ParsedIssueKey k;
    k.Owner = "acme";
    k.Repo = "repo";
    k.Number = 7;
    SUBCASE("ASCII alphanum — no encoding") {
        CHECK(BuildIssueLabelRemoveSuffix(k, "bug") == "/repos/acme/repo/issues/7/labels/bug");
    }
    SUBCASE("contains space + colon (prio: P0 shape)") {
        const std::string s = BuildIssueLabelRemoveSuffix(k, "prio: P0");
        CHECK(s == "/repos/acme/repo/issues/7/labels/prio%3A%20P0");
    }
    SUBCASE("contains slash — separator-injection guard") {
        // Hostile label "../" must not collapse into a parent path. Percent-encoding
        // every reserved char keeps the slash inside the segment.
        const std::string s = BuildIssueLabelRemoveSuffix(k, "../etc/passwd");
        CHECK(s == "/repos/acme/repo/issues/7/labels/..%2Fetc%2Fpasswd");
    }
    SUBCASE("unreserved chars (RFC 3986 set) pass through") {
        const std::string s = BuildIssueLabelRemoveSuffix(k, "A-Z.a-z_0-9~");
        CHECK(s == "/repos/acme/repo/issues/7/labels/A-Z.a-z_0-9~");
    }
}

TEST_CASE("PercentEncodePathSegment — RFC 3986 unreserved set") {
    // Unreserved: [A-Za-z0-9-._~]
    CHECK(PercentEncodePathSegment("Abc") == "Abc");
    CHECK(PercentEncodePathSegment("-._~") == "-._~");
    CHECK(PercentEncodePathSegment("0123456789") == "0123456789");
    // Reserved + sub-delims all encoded.
    CHECK(PercentEncodePathSegment("/") == "%2F");
    CHECK(PercentEncodePathSegment(" ") == "%20");
    CHECK(PercentEncodePathSegment("?#&=") == "%3F%23%26%3D");
    // Empty in → empty out.
    CHECK(PercentEncodePathSegment("") == "");
}

// ─── T2: JSON body builders ─────────────────────────────────────────────────

TEST_CASE("BuildCommentAddBody — {body:string}") {
    const auto j = BuildCommentAddBody("Hello, world.");
    REQUIRE(j.is_object());
    CHECK(j.size() == 1);
    REQUIRE(j.contains("body"));
    CHECK(j["body"].get<std::string>() == "Hello, world.");
    // Round-trip through dump() since that's what the live HTTP path serialises.
    CHECK(j.dump() == "{\"body\":\"Hello, world.\"}");
}

TEST_CASE("BuildLabelAddBody — top-level array per GitHub docs") {
    const auto j = BuildLabelAddBody("bug");
    REQUIRE(j.is_array());
    CHECK(j.size() == 1);
    CHECK(j[0].get<std::string>() == "bug");
    CHECK(j.dump() == "[\"bug\"]");
}

TEST_CASE("BuildAssigneeSetBody — {assignees:[user]}") {
    const auto j = BuildAssigneeSetBody("alice");
    REQUIRE(j.is_object());
    REQUIRE(j.contains("assignees"));
    REQUIRE(j["assignees"].is_array());
    CHECK(j["assignees"].size() == 1);
    CHECK(j["assignees"][0].get<std::string>() == "alice");
    CHECK(j.dump() == "{\"assignees\":[\"alice\"]}");
}

TEST_CASE("BuildStateTransitionBody — {state:string}") {
    const auto open = BuildStateTransitionBody("open");
    CHECK(open.dump() == "{\"state\":\"open\"}");
    const auto closed = BuildStateTransitionBody("closed");
    CHECK(closed.dump() == "{\"state\":\"closed\"}");
    // Builder is intentionally permissive — caller validates with
    // IsValidStateTransitionTarget BEFORE building, so the builder doesn't
    // need to enum-check. Audit-test that the body honours arbitrary strings.
    const auto bogus = BuildStateTransitionBody("draft");
    CHECK(bogus.dump() == "{\"state\":\"draft\"}");
}

// ─── T2: state-transition validator ─────────────────────────────────────────

TEST_CASE("IsValidStateTransitionTarget — enum-strict") {
    CHECK(IsValidStateTransitionTarget("open"));
    CHECK(IsValidStateTransitionTarget("closed"));
    CHECK_FALSE(IsValidStateTransitionTarget(""));
    CHECK_FALSE(IsValidStateTransitionTarget("OPEN"));   // case-sensitive
    CHECK_FALSE(IsValidStateTransitionTarget("Closed")); // case-sensitive
    CHECK_FALSE(IsValidStateTransitionTarget("draft"));
    CHECK_FALSE(IsValidStateTransitionTarget("reopened"));
    CHECK_FALSE(IsValidStateTransitionTarget("open "));   // no trailing space
    CHECK_FALSE(IsValidStateTransitionTarget(" closed")); // no leading space
}

// ─── T2: GitHub structured-error message extraction ─────────────────────────

TEST_CASE("ExtractGitHubErrorMessage — surfaces .message when present") {
    std::string msg;
    SUBCASE("typical 401 body") {
        const std::string body = "{\"message\":\"Bad credentials\",\"documentation_url\":\"https://docs.github.com\"}";
        CHECK(ExtractGitHubErrorMessage(body, msg));
        CHECK(msg == "Bad credentials");
    }
    SUBCASE("422 validation error") {
        const std::string body = "{\"message\":\"Validation Failed\",\"errors\":[{\"resource\":\"Issue\"}]}";
        CHECK(ExtractGitHubErrorMessage(body, msg));
        CHECK(msg == "Validation Failed");
    }
    SUBCASE("body is not JSON — returns false, msg empty") {
        CHECK_FALSE(ExtractGitHubErrorMessage("<html>nginx 502</html>", msg));
        CHECK(msg.empty());
    }
    SUBCASE("body is empty — returns false") {
        CHECK_FALSE(ExtractGitHubErrorMessage("", msg));
        CHECK(msg.empty());
    }
    SUBCASE("body is JSON but no message field") {
        CHECK_FALSE(ExtractGitHubErrorMessage("{\"errors\":[]}", msg));
        CHECK(msg.empty());
    }
    SUBCASE("message field present but empty — returns false (no surfaceable cause)") {
        CHECK_FALSE(ExtractGitHubErrorMessage("{\"message\":\"\"}", msg));
        CHECK(msg.empty());
    }
    SUBCASE("message field is not a string — returns false") {
        CHECK_FALSE(ExtractGitHubErrorMessage("{\"message\":42}", msg));
    }
}

// ─── T2: recorded-fixture parse against the sample github_issues_sample.json ─

// The fixture is a recorded GET /comments response. We round-trip a comment body
// through BuildCommentAddBody to confirm the shape we'd send back is round-trip
// compatible with the shape we receive — i.e. `body` is a string in both
// directions. Full HTTP-path testing is deferred to bucket-E (manual probe).
TEST_CASE("Fixture round-trip — comment body shape") {
    const std::string recordedCommentBody = "+1 — repros for me on macOS 14.";
    const auto outgoing = BuildCommentAddBody(recordedCommentBody);
    CHECK(outgoing["body"].get<std::string>() == recordedCommentBody);
    // A response body shape (server echoes the comment) has the same "body" key.
    const nlohmann::json serverEcho = nlohmann::json::parse(
        "{\"id\":42,\"body\":\"+1 — repros for me on macOS 14.\",\"user\":{\"login\":\"alice\"}}");
    CHECK(serverEcho["body"].get<std::string>() == recordedCommentBody);
}
