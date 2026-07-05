// JiraErrorMessagePure tests — pin ExtractJiraErrorMessage's contract (the Jira-side
// counterpart of ExtractLinearErrorMessage): parse Jira's errorMessages[] + errors{},
// never echo the raw body, fall back to "HTTP <status>", survive depth bombs via the
// bounded parse, and cap the joined message length.

#include "Tracker/JiraErrorMessagePure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::jira::ExtractJiraErrorMessage;

TEST_CASE("errorMessages[] strings join after the HTTP status") {
    const std::string body = R"({"errorMessages":["Field 'priority' is required.","Summary too long."],"errors":{}})";
    CHECK(ExtractJiraErrorMessage(400, body) ==
          "HTTP 400: Field 'priority' is required.; Summary too long.");
}

TEST_CASE("errors{} object values join as 'field: message'") {
    const std::string body = R"({"errorMessages":[],"errors":{"assignee":"User does not exist."}})";
    CHECK(ExtractJiraErrorMessage(400, body) == "HTTP 400: assignee: User does not exist.");
}

TEST_CASE("fallback paths return plain HTTP status") {
    // Empty body.
    CHECK(ExtractJiraErrorMessage(502, "") == "HTTP 502");
    // Non-JSON body (HTML error page) — the raw body must NOT leak into the result.
    const std::string html = "<html><body>Service Unavailable</body></html>";
    CHECK(ExtractJiraErrorMessage(503, html) == "HTTP 503");
    // Valid JSON without Jira's error fields.
    CHECK(ExtractJiraErrorMessage(500, R"({"message":"boom"})") == "HTTP 500");
    // JSON array at top level (not an object).
    CHECK(ExtractJiraErrorMessage(500, "[1,2,3]") == "HTTP 500");
}

TEST_CASE("depth bomb is discarded by the bounded parse, not recursed") {
    std::string bomb;
    for (int i = 0; i < 20000; ++i) {
        bomb += '[';
    }
    // Must return (fallback), not crash — ParseBoundedOrDiscarded rejects before recursion.
    CHECK(ExtractJiraErrorMessage(400, bomb) == "HTTP 400");
}

TEST_CASE("joined message is length-capped") {
    std::string body = R"({"errorMessages":[)";
    for (int i = 0; i < 50; ++i) {
        if (i != 0) {
            body += ',';
        }
        body += R"("This is a fairly long validation message used to overflow the cap.")";
    }
    body += "]}";
    const std::string out = ExtractJiraErrorMessage(400, body);
    // "HTTP 400: " prefix + capped joined text; generous upper bound well under the raw size.
    CHECK(out.size() < 450);
    CHECK(out.compare(0, 9, "HTTP 400:") == 0);
}

TEST_CASE("non-string entries are skipped, not crashed on") {
    const std::string body = R"({"errorMessages":[42,null,"real message"],"errors":{"f":123}})";
    CHECK(ExtractJiraErrorMessage(400, body) == "HTTP 400: real message");
}
