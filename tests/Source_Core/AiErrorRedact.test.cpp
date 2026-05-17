#include <doctest/doctest.h>

#include "AiErrorRedact.h"

using smatchet::ai::pure::kMaxProviderErrorBodyChars;
using smatchet::ai::pure::RedactProviderErrorBody;

TEST_CASE("RedactProviderErrorBody strips Bearer tokens" * doctest::test_suite("[high-risk]")) {
    SUBCASE("Bearer in plain header echo") {
        const std::string out = RedactProviderErrorBody("Authorization header was: Bearer sk-abc123def456ghi789jkl");
        CHECK(out.find("sk-abc123") == std::string::npos);
        CHECK(out.find("Bearer [REDACTED]") != std::string::npos);
    }

    SUBCASE("Bearer inside JSON body") {
        const std::string body = R"({"echoed_header":"Bearer sk-supersecrettoken1234567"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("supersecrettoken") == std::string::npos);
        CHECK(out.find("[REDACTED]") != std::string::npos);
    }

    SUBCASE("Multiple Bearer tokens all redacted") {
        const std::string body = "first=Bearer aaaaaaaa second=Bearer bbbbbbbb";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("aaaaaaaa") == std::string::npos);
        CHECK(out.find("bbbbbbbb") == std::string::npos);
    }
}

TEST_CASE("RedactProviderErrorBody strips api_key / Authorization JSON fields" * doctest::test_suite("[high-risk]")) {
    SUBCASE("snake_case api_key field") {
        const std::string body = R"({"error":"invalid","api_key":"sk-mysecret1234567890abcdef"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("mysecret") == std::string::npos);
        CHECK(out.find("\"api_key\":\"[REDACTED]\"") != std::string::npos);
    }

    SUBCASE("camelCase apiKey field") {
        const std::string body = R"({"apiKey":"mykey1234567890"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("mykey") == std::string::npos);
    }

    SUBCASE("Authorization field (capitalised)") {
        const std::string body = R"({"Authorization":"Bearer sk-leak123"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("leak123") == std::string::npos);
    }

    SUBCASE("lowercase authorization field") {
        const std::string body = R"({"authorization":"sk-anotherleak"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("anotherleak") == std::string::npos);
    }

    SUBCASE("Field with whitespace before colon") {
        const std::string body = R"({"api_key"   :   "sk-spaced1234567890"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("spaced") == std::string::npos);
    }
}

TEST_CASE("RedactProviderErrorBody strips id-prefix tokens (sk-/org-/proj_/asst_)" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("standalone sk- token") {
        const std::string out = RedactProviderErrorBody("error referenced sk-abc12345xyzdefg");
        CHECK(out.find("abc12345xyzdefg") == std::string::npos);
        CHECK(out.find("sk-[REDACTED]") != std::string::npos);
    }

    SUBCASE("org- identifier") {
        const std::string out = RedactProviderErrorBody("org=org-acmeincorp12345");
        CHECK(out.find("acmeincorp12345") == std::string::npos);
    }

    SUBCASE("proj_ identifier") {
        const std::string out = RedactProviderErrorBody("project: proj_alphabravoone1234");
        CHECK(out.find("alphabravoone1234") == std::string::npos);
    }

    SUBCASE("asst_ identifier") {
        const std::string out = RedactProviderErrorBody("assistant=asst_zulufoxtrot12345");
        CHECK(out.find("zulufoxtrot12345") == std::string::npos);
    }

    SUBCASE("Short suffix (<8 chars) is left alone — not enough entropy to be a secret") {
        const std::string out = RedactProviderErrorBody("see sk-abc");
        CHECK(out.find("sk-abc") != std::string::npos);
    }
}

TEST_CASE("RedactProviderErrorBody length-caps after sanitisation" * doctest::test_suite("[high-risk]")) {
    SUBCASE("Body shorter than cap passes through unchanged when no secrets") {
        const std::string body = "Plain error: invalid_request: bad temperature value";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out == body);
        CHECK(out.size() < kMaxProviderErrorBodyChars);
    }

    SUBCASE("Body longer than cap is truncated with elipsis suffix") {
        const std::string body(kMaxProviderErrorBodyChars * 2, 'X');
        const std::string out = RedactProviderErrorBody(body);
        // sizeof("…") in UTF-8 is 3 bytes; cap byte length + 3 expected.
        CHECK(out.size() == kMaxProviderErrorBodyChars + 3);
        CHECK(out.substr(kMaxProviderErrorBodyChars) == "…");
    }

    SUBCASE("Long body with embedded secret — both truncate AND redact apply") {
        std::string body = R"({"api_key":"sk-leaked12345abcdef","filler":")";
        body.append(kMaxProviderErrorBodyChars * 2, 'X');
        body.append("\"}");
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("leaked") == std::string::npos);
        CHECK(out.size() <= kMaxProviderErrorBodyChars + 3);
    }
}

TEST_CASE("RedactProviderErrorBody handles benign inputs safely") {
    SUBCASE("Empty body returns empty") { CHECK(RedactProviderErrorBody("").empty()); }

    SUBCASE("Body with no secrets returns verbatim") {
        const std::string body = R"({"error":{"message":"max_tokens exceeded","type":"invalid_request_error"}})";
        CHECK(RedactProviderErrorBody(body) == body);
    }

    SUBCASE("Whitespace-only body returns whitespace-only") {
        CHECK(RedactProviderErrorBody("   \n\t   ") == "   \n\t   ");
    }

    SUBCASE("Bearer with empty token doesn't crash") {
        const std::string out = RedactProviderErrorBody("Bearer ");
        CHECK(out.find("Bearer") != std::string::npos);
    }
}
