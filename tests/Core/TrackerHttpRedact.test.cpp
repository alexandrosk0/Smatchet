#include <doctest/doctest.h>

#include "TrackerHttpPure.h"

#include <string>

// DR30: the tracker error paths (Jira AddIssueWatcher, Plane fetch/resolve) now
// route the raw HTTP response body through RedactHttpBodyForLog before splicing it
// into the user-facing error string and the ERROR log. A 401/403 body can reflect
// the request's Authorization / x-api-key, so the raw body must never be logged.
TEST_CASE("RedactHttpBodyForLog strips a reflected Bearer token (DR30)") {
    const std::string out = RedactHttpBodyForLog("Authorization was: Bearer sk-abc123def456ghi789jkl");
    CHECK(out.find("sk-abc123") == std::string::npos);
    CHECK(out.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("RedactHttpBodyForLog strips an api_key JSON field (DR30)") {
    const std::string out = RedactHttpBodyForLog(R"({"error":"invalid","api_key":"sk-mysecret1234567890abcdef"})");
    CHECK(out.find("mysecret") == std::string::npos);
    CHECK(out.find("[REDACTED]") != std::string::npos);
}
