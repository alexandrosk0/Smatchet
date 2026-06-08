#include <doctest/doctest.h>

#include "OllamaStreamError.h"

using smatchet::ai::pure::FormatOllamaHttpError;
using smatchet::ai::pure::FormatOllamaTransportError;

TEST_CASE("FormatOllamaTransportError prefixes the cpr message") {
    SUBCASE("typical connection-refused message") {
        const std::string out = FormatOllamaTransportError("Failed to connect to localhost port 11434");
        CHECK(out == "transport: Failed to connect to localhost port 11434");
    }

    SUBCASE("empty message keeps the prefix") { CHECK(FormatOllamaTransportError("") == "transport: "); }
}

TEST_CASE("FormatOllamaHttpError formats status with optional redacted body") {
    SUBCASE("non-empty body is appended after the status") {
        const std::string out = FormatOllamaHttpError(404, "model not found");
        CHECK(out == "HTTP 404: model not found");
    }

    SUBCASE("empty body yields status only — no trailing colon") {
        const std::string out = FormatOllamaHttpError(500, "");
        CHECK(out == "HTTP 500");
    }

    SUBCASE("status is rendered verbatim for 4xx/5xx") {
        CHECK(FormatOllamaHttpError(429, "rate limited") == "HTTP 429: rate limited");
        CHECK(FormatOllamaHttpError(401, "unauthorized") == "HTTP 401: unauthorized");
    }
}

// FormatOllamaHttpError is pure assembly — it does NOT redact (the caller
// sanitises once, for both the log line and this message; see OllamaClient
// SendStreaming). It must therefore surface the already-sanitised body
// verbatim, never re-mangling a "[REDACTED]" placeholder the caller produced.
TEST_CASE("FormatOllamaHttpError surfaces the pre-sanitised body verbatim") {
    SUBCASE("already-redacted body is appended unchanged") {
        const std::string out = FormatOllamaHttpError(401, "auth failed: Bearer [REDACTED]");
        CHECK(out == "HTTP 401: auth failed: Bearer [REDACTED]");
    }

    SUBCASE("body with no secrets passes through") {
        const std::string out = FormatOllamaHttpError(400, R"({"error":"invalid_request"})");
        CHECK(out == R"(HTTP 400: {"error":"invalid_request"})");
    }
}
