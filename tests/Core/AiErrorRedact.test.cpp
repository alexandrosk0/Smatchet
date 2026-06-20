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

    SUBCASE("Anthropic x-api-key field echo") {
        const std::string body = R"({"echoed_header":{"x-api-key":"sk-ant-leakedanthropickey1234"}})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("leakedanthropickey") == std::string::npos);
        CHECK(out.find("\"x-api-key\":\"[REDACTED]\"") != std::string::npos);
    }

    SUBCASE("X-Api-Key capitalised header echo") {
        const std::string body = R"({"X-Api-Key":"sk-mixedcaseleak1234567"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("mixedcaseleak") == std::string::npos);
    }

    SUBCASE("anthropic-api-key field echo") {
        const std::string body = R"({"anthropic-api-key":"sk-ant-otherleak1234567"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("otherleak") == std::string::npos);
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

// Bundle B SH3 — GitHub PAT prefixes must redact alongside OpenAI id prefixes.
// A 401 from GitHub that echoes the supplied token verbatim (rare but
// observed in some validation error paths) must not surface to LOG_WARN or
// AiStreamError::Message in plaintext.
TEST_CASE("RedactProviderErrorBody strips GitHub PAT prefixes (ghp_/gho_/ghs_/ghu_/ghr_/github_pat_)" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("Classic PAT prefix ghp_") {
        const std::string out = RedactProviderErrorBody("token=ghp_abcdefghij1234567890");
        CHECK(out.find("abcdefghij1234567890") == std::string::npos);
        CHECK(out.find("ghp_[REDACTED]") != std::string::npos);
    }
    SUBCASE("OAuth token prefix gho_") {
        const std::string out = RedactProviderErrorBody("Authorization: token gho_oauthsecretvalue1234");
        CHECK(out.find("oauthsecretvalue1234") == std::string::npos);
    }
    SUBCASE("GitHub App server-token prefix ghs_") {
        const std::string out = RedactProviderErrorBody("server token: ghs_servervalue9876543210");
        CHECK(out.find("servervalue9876543210") == std::string::npos);
    }
    SUBCASE("User-to-server token prefix ghu_") {
        const std::string out = RedactProviderErrorBody("u2s: ghu_useruserusersecret123");
        CHECK(out.find("useruserusersecret123") == std::string::npos);
    }
    SUBCASE("Refresh token prefix ghr_") {
        const std::string out = RedactProviderErrorBody("refresh: ghr_refreshrefresh1234567");
        CHECK(out.find("refreshrefresh1234567") == std::string::npos);
    }
    SUBCASE("Fine-grained PAT prefix github_pat_") {
        const std::string body = "fg-pat=github_pat_abcdefghijklmnopqrstuvwxyz1234567890";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("abcdefghijklmnop") == std::string::npos);
        CHECK(out.find("github_pat_[REDACTED]") != std::string::npos);
    }
    SUBCASE("PAT echoed in JSON field github_pat") {
        const std::string body = R"({"github_pat":"ghp_secretpatvalue1234567"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("secretpatvalue") == std::string::npos);
    }
    SUBCASE("PAT echoed in JSON field GitHubPat (config field name shape)") {
        const std::string body = R"({"GitHubPat":"ghp_anothersecret12345678"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("anothersecret") == std::string::npos);
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

// security synthesis #11 — the AI clients (OpenAI/Anthropic/Ollama/Whisper) MUST
// NOT follow redirects, so a cross-host 30x can never forward the provider key
// (Bearer / x-api-key) as a caller-set raw header. The redirect call itself is
// cpr-bound (untestable in the cpr-free rig), so pin the policy constant here.
TEST_CASE("AI clients never follow redirects (synthesis #11)" * doctest::test_suite("[high-risk]")) {
    CHECK(smatchet::ai::pure::kAiFollowRedirects == false);
}

// security synthesis #12 — tracker HTTP body logging (RedactHttpBodyForLog, which
// delegates to RedactProviderErrorBody) must strip a reflected credential before
// it reaches a log line. RedactHttpBodyForLog is cpr-bound; assert the delegated
// redaction covers the tracker-shaped reflections (Basic-auth echo, PAT).
TEST_CASE("Tracker error-body reflections are redacted before logging (synthesis #12)" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("Jira 401 echoing the raw Authorization header") {
        const std::string body = R"({"errorMessages":["bad creds"],"Authorization":"Basic dXNlcjpzZWNyZXR0b2tlbg=="})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("dXNlcjpzZWNyZXR0b2tlbg==") == std::string::npos);
        CHECK(out.find("\"Authorization\":\"[REDACTED]\"") != std::string::npos);
    }
    SUBCASE("GitHub error body reflecting the PAT verbatim") {
        const std::string out = RedactProviderErrorBody("Bad credentials for token ghp_reflectedpat1234567890");
        CHECK(out.find("reflectedpat1234567890") == std::string::npos);
        CHECK(out.find("ghp_[REDACTED]") != std::string::npos);
    }
}

// SSE/NDJSON parse-failure log-line hardening — the AnthropicClient / OpenAiClient /
// OllamaClient parse-failure paths now route the raw `data` / `rawLine` excerpt through
// RedactProviderErrorBody before LOG_WARN. A misconfigured proxy can echo the request
// Authorization header into a malformed stream chunk; redaction strips it before it reaches
// the log. The LOG_WARN call is cpr/stream-bound (integration-only), so pin the delegated
// redaction over the exact shapes a malformed stream chunk would carry.
TEST_CASE("Malformed SSE/NDJSON stream chunk redacts a reflected Authorization header before logging" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("Bearer token echoed in a malformed SSE data line") {
        const std::string chunk = "<html>502 Bad Gateway: Authorization: Bearer sk-proxyleaked1234567890</html>";
        const std::string out = RedactProviderErrorBody(chunk);
        CHECK(out.find("proxyleaked1234567890") == std::string::npos);
        CHECK(out.find("Bearer [REDACTED]") != std::string::npos);
    }
    SUBCASE("x-api-key echoed in a malformed NDJSON line") {
        const std::string line = R"({"not":"json-the-stream-broke","x-api-key":"sk-ant-ndjsonleak1234567"})";
        const std::string out = RedactProviderErrorBody(line);
        CHECK(out.find("ndjsonleak") == std::string::npos);
    }
}

// issue #1286 — the AnthropicClient / OpenAiClient SSE parse-failure paths now route the
// nlohmann parse_error::what() text through RedactProviderErrorBody before LOG_WARN, not
// just the body excerpt. parse_error::what() embeds the offending input window
// ("… last read: '<frag>'"); a truncated stream that splits an Authorization header
// mid-token would otherwise write a live credential to the log verbatim. Pin the redaction
// over the exact shape what() carries.
TEST_CASE("RedactProviderErrorBody scrubs a credential embedded in nlohmann parse_error::what()" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("Bearer token inside a what() 'last read' window") {
        const std::string what = "[json.exception.parse_error.101] parse error at line 1, column 64: "
                                 "syntax error while parsing value - unexpected end of input; "
                                 "last read: 'Authorization: Bearer sk-ant-leakedfromwhat1234567'";
        const std::string out = RedactProviderErrorBody(what);
        CHECK(out.find("leakedfromwhat") == std::string::npos);
        CHECK(out.find("Bearer [REDACTED]") != std::string::npos);
    }
    SUBCASE("x-api-key header fragment inside a what() window") {
        const std::string what = "parse error - invalid literal; last read: 'x-api-key: weirdshapedkeyvalue123'";
        const std::string out = RedactProviderErrorBody(what);
        CHECK(out.find("weirdshapedkeyvalue123") == std::string::npos);
    }
}

// issue #1286 secondary — Basic-auth credentials and raw (unquoted) x-api-key header lines
// must redact regardless of the `sk-` prefix sweep, and the prefix sweep itself must be
// case-insensitive so an upper-cased reflection can't slip a token through.
TEST_CASE("RedactProviderErrorBody strips Basic-auth and raw api-key header lines (issue #1286)" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("Authorization: Basic <b64> raw header echo") {
        const std::string body = "upstream 502: Authorization: Basic dXNlcjpzdXBlcnNlY3JldHRva2Vu\r\n";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("dXNlcjpzdXBlcnNlY3JldHRva2Vu") == std::string::npos);
        CHECK(out.find("Basic [REDACTED]") != std::string::npos);
    }
    SUBCASE("Basic <b64> inside a JSON string value still fully redacts") {
        const std::string body = R"({"echoed":"Basic dXNlcjpwYXNzd29yZGxlYWs="})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("dXNlcjpwYXNzd29yZGxlYWs") == std::string::npos);
    }
    SUBCASE("raw x-api-key header line with a non-sk- prefixed key (any-prefix scrub)") {
        const std::string body = "X-Api-Key: zzqq-rotated-key-shape-99887766\nbody follows";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("zzqq-rotated-key-shape-99887766") == std::string::npos);
        CHECK(out.find("body follows") != std::string::npos); // value scrub stops at the newline
    }
    SUBCASE("quoted x-api-key field is left to the JSON scrubber (no double redaction)") {
        const std::string body = R"({"x-api-key":"sk-ant-keepjsonshape1234567"})";
        const std::string out = RedactProviderErrorBody(body);
        CHECK(out.find("keepjsonshape") == std::string::npos);
        CHECK(out.find("\"x-api-key\":\"[REDACTED]\"") != std::string::npos);
    }
    SUBCASE("upper-cased SK- prefix is still redacted (case-insensitive sweep)") {
        const std::string out = RedactProviderErrorBody("token referenced SK-UPPERCASEDLEAK123456");
        CHECK(out.find("UPPERCASEDLEAK123456") == std::string::npos);
    }
    SUBCASE("upper-cased GHP_ PAT prefix is still redacted") {
        const std::string out = RedactProviderErrorBody("pat=GHP_UPPERCASEPATVALUE1234567");
        CHECK(out.find("UPPERCASEPATVALUE1234567") == std::string::npos);
    }
}
