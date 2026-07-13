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
    CHECK(ExtractJiraErrorMessage(400, body) == "HTTP 400: Field 'priority' is required.; Summary too long.");
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

TEST_CASE("cap boundary: a huge second message cannot blow past the cap via the separator") {
    // Regression: the separator used to be appended before the cap math, letting the
    // subtraction underflow and splice the entire (attacker-sized) next message.
    std::string first(399, 'a');
    std::string huge(100000, 'b');
    const std::string body = std::string(R"({"errorMessages":[")") + first + R"(",")" + huge + R"("]})";
    const std::string out = ExtractJiraErrorMessage(400, body);
    CHECK(out.size() < 450);
}

TEST_CASE("cap never splits a multi-byte UTF-8 sequence") {
    // 300 x 'é' (2 bytes each) = 600 bytes of message — past the 400-byte joined cap, so the
    // truncation branch actually runs and its lead-byte backoff is what's under test. (The
    // previous 200 x 'é' = exactly-400-byte message fit the `<= cap` append whole, so the
    // backoff loop never executed and a mutant of it survived — MUTATION_PILOT.md Phase 3
    // JIRAERR-02.) The cut must land on a lead-byte boundary.
    std::string msg;
    for (int i = 0; i < 300; ++i) {
        msg += "\xC3\xA9";
    }
    const std::string body = std::string(R"({"errorMessages":[")") + msg + R"("]})";
    const std::string out = ExtractJiraErrorMessage(400, body);
    // Truncation must actually have engaged (otherwise this test asserts nothing about the
    // backoff): the ellipsis marker is appended only on the truncation path.
    CHECK(out.size() >= 3);
    CHECK(out.compare(out.size() - 3, 3, "\xE2\x80\xA6") == 0);
    // No byte in the result may be a lone continuation byte after a non-lead byte cut;
    // simplest check: the char before the ellipsis must not be a UTF-8 lead byte expecting
    // a continuation that never came. Verify by scanning for validity.
    int expectCont = 0;
    bool valid = true;
    for (unsigned char c : out) {
        if (expectCont > 0) {
            if ((c & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
            --expectCont;
        } else if ((c & 0x80u) == 0) {
            // ASCII
        } else if ((c & 0xE0u) == 0xC0u) {
            expectCont = 1;
        } else if ((c & 0xF0u) == 0xE0u) {
            expectCont = 2;
        } else if ((c & 0xF8u) == 0xF0u) {
            expectCont = 3;
        } else {
            valid = false;
            break;
        }
    }
    CHECK(valid);
    CHECK(expectCont == 0);
}

TEST_CASE("non-string entries are skipped, not crashed on") {
    const std::string body = R"({"errorMessages":[42,null,"real message"],"errors":{"f":123}})";
    CHECK(ExtractJiraErrorMessage(400, body) == "HTTP 400: real message");
}
