// LinearClientHelpers doctest — pure helper round-trips.
// Slice 1 of docs/plans/linear-tracker-backend.md.

#include "LinearClientHelpers.h"

#include <doctest/doctest.h>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

using namespace smatchet::linear;

TEST_CASE("ParseLinearIssueKey — canonical TEAM-123") {
    ParsedLinearIssueKey k;
    REQUIRE(ParseLinearIssueKey("ENG-42", k));
    CHECK(k.TeamKey == "ENG");
    CHECK(k.Number == 42);

    ParsedLinearIssueKey k2;
    REQUIRE(ParseLinearIssueKey("ABC2-999999", k2)); // team key may contain digits
    CHECK(k2.TeamKey == "ABC2");
    CHECK(k2.Number == 999999);
}

TEST_CASE("ParseLinearIssueKey — rejects malformed identifiers") {
    ParsedLinearIssueKey k;
    CHECK_FALSE(ParseLinearIssueKey("", k));
    CHECK_FALSE(ParseLinearIssueKey("ENG", k));     // no dash
    CHECK_FALSE(ParseLinearIssueKey("-1", k));      // empty team key
    CHECK_FALSE(ParseLinearIssueKey("ENG-", k));    // empty number
    CHECK_FALSE(ParseLinearIssueKey("ENG-abc", k)); // non-numeric
    CHECK_FALSE(ParseLinearIssueKey("ENG-0", k));   // non-positive
    CHECK_FALSE(ParseLinearIssueKey("EN G-1", k));  // space
    CHECK_FALSE(ParseLinearIssueKey("2NG-1", k));   // team key must start with a letter
}

TEST_CASE("ParseLinearIssueKey — rejects an int64-overflowing number instead of wrapping (UB)") {
    // CPP_CODE_AUDIT.md #18: an unbounded digit run used to accumulate past INT64_MAX
    // (signed-overflow UB) instead of being rejected as an invalid key.
    ParsedLinearIssueKey k;
    CHECK_FALSE(ParseLinearIssueKey("ENG-99999999999999999999", k));
    // one past int64 max is rejected too
    CHECK_FALSE(ParseLinearIssueKey("ENG-9223372036854775808", k));
    // int64 max itself still parses cleanly (boundary, not off-by-one)
    ParsedLinearIssueKey kMax;
    REQUIRE(ParseLinearIssueKey("ENG-9223372036854775807", kMax));
    CHECK(kMax.Number == 9223372036854775807LL);
}

TEST_CASE("FormatLinearIssueKey — round-trips with the parser") {
    const std::string key = FormatLinearIssueKey("ENG", 7);
    CHECK(key == "ENG-7");
    ParsedLinearIssueKey k;
    REQUIRE(ParseLinearIssueKey(key, k));
    CHECK(k.TeamKey == "ENG");
    CHECK(k.Number == 7);
}

TEST_CASE("NormalizeLinearApiUrl — empty defaults, trailing slash trimmed") {
    CHECK(NormalizeLinearApiUrl("") == std::string(kDefaultLinearApiUrl));
    CHECK(NormalizeLinearApiUrl("https://api.linear.app/graphql/") == "https://api.linear.app/graphql");
    CHECK(NormalizeLinearApiUrl("https://api.linear.app/graphql") == "https://api.linear.app/graphql");
    CHECK(NormalizeLinearApiUrl("///") == std::string(kDefaultLinearApiUrl));
}

TEST_CASE("IsValidLinearApiUrl — accepts empty + https, rejects http + non-url") {
    CHECK(IsValidLinearApiUrl("").has_value()); // empty -> default
    CHECK(IsValidLinearApiUrl("https://api.linear.app/graphql").has_value());
    CHECK_FALSE(IsValidLinearApiUrl("http://api.linear.app/graphql").has_value()); // not https
    CHECK_FALSE(IsValidLinearApiUrl("api.linear.app").has_value());                // no scheme
    CHECK_FALSE(IsValidLinearApiUrl("https://").has_value());                      // no host
}

TEST_CASE("BuildGraphQLBody — embeds query + variables; empty vars -> object") {
    nlohmann::json vars;
    vars["teamId"] = "abc";
    const std::string body = BuildGraphQLBody("query { viewer { id } }", vars);
    nlohmann::json parsed = nlohmann::json::parse(body);
    CHECK(parsed["query"] == "query { viewer { id } }");
    CHECK(parsed["variables"]["teamId"] == "abc");

    const std::string emptyVars = BuildGraphQLBody("query {}", nlohmann::json());
    nlohmann::json parsedEmpty = nlohmann::json::parse(emptyVars);
    CHECK(parsedEmpty["variables"].is_object());
    CHECK(parsedEmpty["variables"].empty());
}

TEST_CASE("BuildLinearAuthHeaderValue — raw key, NO Bearer prefix") {
    CHECK(BuildLinearAuthHeaderValue("lin_api_xxx") == "lin_api_xxx");
    CHECK(BuildLinearAuthHeaderValue("lin_api_xxx").find("Bearer") == std::string::npos);
}

TEST_CASE("LinearResponseHasErrors — 200-with-errors detected + joined") {
    std::string msg;
    nlohmann::json withErrors = nlohmann::json::parse(
        R"({"errors":[{"message":"Authentication required"},{"message":"Bad team"}],"data":null})");
    REQUIRE(LinearResponseHasErrors(withErrors, msg));
    CHECK(msg == "Authentication required; Bad team");

    nlohmann::json dataOnly = nlohmann::json::parse(R"({"data":{"viewer":{"id":"u1"}}})");
    CHECK_FALSE(LinearResponseHasErrors(dataOnly, msg));
    CHECK(msg.empty());

    nlohmann::json emptyErrors = nlohmann::json::parse(R"({"errors":[]})");
    CHECK_FALSE(LinearResponseHasErrors(emptyErrors, msg));
}

TEST_CASE("ExtractLinearErrorMessage — errors win, else HTTP status fallback") {
    CHECK(ExtractLinearErrorMessage(200, R"({"errors":[{"message":"Rate limited"}]})") == "Rate limited");
    CHECK(ExtractLinearErrorMessage(401, R"({"data":{"viewer":null}})") == "HTTP 401");
    CHECK(ExtractLinearErrorMessage(500, "not json at all") == "HTTP 500");
    CHECK(ExtractLinearErrorMessage(503, "") == "HTTP 503");
}

TEST_CASE("ParseLinearRateLimitHeaders — parses the documented headers") {
    std::map<std::string, std::string> headers;
    headers["x-ratelimit-requests-limit"] = "5000";
    headers["x-ratelimit-requests-remaining"] = "4987";
    headers["x-ratelimit-requests-reset"] = "1718900000";
    headers["x-ratelimit-complexity-limit"] = "3000000";
    headers["x-ratelimit-complexity-remaining"] = "2999000";
    headers["x-complexity"] = "42";
    LinearRateLimit rl = ParseLinearRateLimitHeaders(headers);
    CHECK(rl.RequestsLimit == 5000);
    CHECK(rl.RequestsRemaining == 4987);
    CHECK(rl.RequestsResetUnixSec == 1718900000);
    CHECK(rl.ComplexityLimit == 3000000);
    CHECK(rl.ComplexityRemaining == 2999000);
    CHECK(rl.Complexity == 42);
    CHECK(rl.Present());

    LinearRateLimit absent = ParseLinearRateLimitHeaders(std::map<std::string, std::string>());
    CHECK(absent.RequestsLimit == -1);
    CHECK_FALSE(absent.Present());
}

TEST_CASE("ParseLinearRateLimitHeaders — an overflowing header falls back instead of wrapping (UB)") {
    // CPP_CODE_AUDIT.md #18: ParseLongOr accumulates into a `long` (32-bit on Windows) from
    // server-controlled headers; an unbounded digit run used to overflow (UB) instead of
    // falling back to the documented "absent" sentinel (-1).
    std::map<std::string, std::string> headers;
    headers["x-complexity"] = "99999999999999999999";
    LinearRateLimit rl = ParseLinearRateLimitHeaders(headers);
    CHECK(rl.Complexity == -1);
}

TEST_CASE("ParseLinearRateLimitHeaders — a negative header keeps its sign (incl. LONG_MIN)") {
    std::map<std::string, std::string> headers;
    headers["x-complexity"] = "-7";
    CHECK(ParseLinearRateLimitHeaders(headers).Complexity == -7);
    // The deliberate LONG_MIN reconstruction (magnitude LONG_MAX+1) round-trips too.
    headers["x-complexity"] = std::to_string((std::numeric_limits<long>::min)());
    CHECK(ParseLinearRateLimitHeaders(headers).Complexity == (std::numeric_limits<long>::min)());
}

TEST_CASE("ResolveLinearRequestAuth — live key authoritative, url falls back") {
    LinearRequestAuth a = ResolveLinearRequestAuth("https://api.linear.app/graphql", "key1", "https://fallback");
    CHECK(a.ApiKey == "key1");
    CHECK(a.ApiUrl == "https://api.linear.app/graphql");

    // Empty live key => empty (user cleared the credential; no ctor-snapshot fallback).
    LinearRequestAuth cleared = ResolveLinearRequestAuth("", "", "https://fallback/graphql");
    CHECK(cleared.ApiKey.empty());
    CHECK(cleared.ApiUrl == "https://fallback/graphql"); // url still falls back

    // No cfg url, no fallback => default endpoint.
    LinearRequestAuth def = ResolveLinearRequestAuth("", "key2", "");
    CHECK(def.ApiUrl == std::string(kDefaultLinearApiUrl));
    CHECK(def.ApiKey == "key2");
}
