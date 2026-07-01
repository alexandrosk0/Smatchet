// AiClientErrorRedact.test.cpp — the `per-client-error-body-redaction-gate`.
//
// Regression gate closing the test.md gap: earlier fixes patched sibling
// AI clients that leaked a reflected API key into AiStreamError::Message because
// each client wires RedactProviderErrorBody manually, with NO test enforcing
// that EVERY IAiClient implementation routes its error body through the redactor.
//
// This drives each REAL production client (OpenAiClient / AnthropicClient /
// OllamaClient) against an in-process httplib server (tests/support/AiHttpFixture.h)
// that returns 401 with the caller's API key echoed verbatim in the body, then
// asserts the resulting AiStreamError::Message does NOT contain the literal key.
// Unlike AiErrorRedact.test.cpp (which pins the pure RedactProviderErrorBody
// helper directly), this exercises the END-TO-END client path: cpr POST → 401 →
// r.text → client wiring → AiStreamError::Message. A new client that forgets the
// redaction wiring fails HERE even though the pure helper still passes.
//
// Pure C++14. cpr + httplib bound — cpr-bearing SmatchetTests rig only.

#include <doctest/doctest.h>

#include "AiHttpFixture.h"

#include "AiTypes.h"
#include "AnthropicClient.h"
#include "IAiClient.h"
#include "OllamaClient.h"
#include "OpenAiClient.h"

#include <atomic>
#include <memory>
#include <string>

namespace {

using smatchet_tests::AiHttpFixture;
using smatchet_tests::AiWireProtocol;

AiCancelToken makeCancelToken() { return std::make_shared<std::atomic<bool>>(false); }

std::unique_ptr<IAiClient> makeClient(AiWireProtocol proto) {
    switch (proto) {
    case AiWireProtocol::OpenAiSse:
        return std::unique_ptr<IAiClient>(new OpenAiClient());
    case AiWireProtocol::AnthropicSse:
        return std::unique_ptr<IAiClient>(new AnthropicClient());
    case AiWireProtocol::OllamaNdjson:
        return std::unique_ptr<IAiClient>(new OllamaClient());
    }
    return std::unique_ptr<IAiClient>();
}

struct ErrorOutcome {
    bool errorFired = false;
    int httpStatus = 0;
    bool wasCancelled = false;
    std::string message;
};

ErrorOutcome run401(IAiClient& client, AiWireProtocol proto, const std::string& key) {
    AiHttpFixture fx;
    fx.ScriptError(401, AiHttpFixture::EchoKey401Body(proto, key));

    AiClientConfig cfg;
    cfg.BaseUrl = fx.BaseUrl();
    cfg.ApiKey = key;
    cfg.ConnectTimeoutMs = 2000;
    cfg.TotalTimeoutMs = 30000;

    AiChatRequest req;
    req.Model = "test-model";
    req.History.push_back(AiMessage());
    req.History.back().Role = "user";
    req.History.back().Content = "hello";

    AiCancelToken cancel = makeCancelToken();
    ErrorOutcome out;
    client.SendStreaming(
        cfg, req, [](const AiStreamDelta&) {},
        [&](const AiStreamError& e) {
            out.errorFired = true;
            out.httpStatus = e.HttpStatus;
            out.wasCancelled = e.WasCancelled;
            out.message = e.Message;
        },
        cancel);
    return out;
}

// The literal key the fixture echoes back. Long + high-entropy so the redactor's
// sk-prefix + Bearer + x-api-key sweeps all have a target; if any survives, the
// substring check below trips.
const char* kLeakedKey = "sk-ant-leakedsecretkey1234567890abcdef";

void checkRedaction(AiWireProtocol proto, const char* label) {
    std::unique_ptr<IAiClient> client = makeClient(proto);
    REQUIRE(client);
    CAPTURE(label);

    const ErrorOutcome out = run401(*client, proto, kLeakedKey);
    CAPTURE(out.message);

    // The error path fired with the upstream 401 surfaced (transport OK).
    CHECK(out.errorFired);
    CHECK(out.httpStatus == 401);
    CHECK_FALSE(out.wasCancelled);

    // THE gate: the literal key must not appear anywhere in the surfaced message.
    CHECK(out.message.find(kLeakedKey) == std::string::npos);
    // Also reject the high-entropy suffix on its own (a partial-prefix-strip
    // bug would leave the secret body behind even if "sk-ant-" was removed).
    CHECK(out.message.find("leakedsecretkey1234567890abcdef") == std::string::npos);
}

} // namespace

TEST_CASE("OpenAiClient: 401 echoing the API key redacts it from AiStreamError::Message" *
          doctest::test_suite("[high-risk][ai-client-http]")) {
    checkRedaction(AiWireProtocol::OpenAiSse, "OpenAi");
}

TEST_CASE("AnthropicClient: 401 echoing the API key redacts it from AiStreamError::Message" *
          doctest::test_suite("[high-risk][ai-client-http]")) {
    checkRedaction(AiWireProtocol::AnthropicSse, "Anthropic");
}

TEST_CASE("OllamaClient: 401 echoing the API key redacts it from AiStreamError::Message" *
          doctest::test_suite("[high-risk][ai-client-http]")) {
    checkRedaction(AiWireProtocol::OllamaNdjson, "Ollama");
}

// Belt-and-braces: the surfaced message still carries the HTTP status so the
// user gets a diagnosable error — redaction must scrub the secret WITHOUT
// blanking the whole body.
TEST_CASE("AI client 401 message preserves the status while redacting the key" *
          doctest::test_suite("[ai-client-http]")) {
    std::unique_ptr<IAiClient> client = makeClient(AiWireProtocol::OpenAiSse);
    REQUIRE(client);
    const ErrorOutcome out = run401(*client, AiWireProtocol::OpenAiSse, kLeakedKey);
    CHECK(out.errorFired);
    CHECK(out.httpStatus == 401);
    CHECK(out.message.find("401") != std::string::npos);
    CHECK(out.message.find(kLeakedKey) == std::string::npos);
}
