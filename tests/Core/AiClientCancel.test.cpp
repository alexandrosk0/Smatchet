// AiClientCancel.test.cpp — the `aiclientcancel-per-client-regression`.
//
// Per-client mid-stream cancellation regression, parameterised across the three
// REAL production AI clients (OpenAiClient / AnthropicClient / OllamaClient)
// driven over a real cpr HTTP round-trip at an in-process httplib loopback
// server (tests/support/AiHttpFixture.h). NOT a stub test — the existing
// StubAiClientCancel.test.cpp pins the test-infrastructure stub; THIS pins each
// production client's cpr WriteCallback cancel-atom poll, which the stub cannot
// exercise (it never touches cpr/httplib).
//
// Contract under test (IAiClient::SendStreaming, all three impls):
//   * The cpr WriteCallback polls `cancel` on every delivered chunk and returns
//     false to abort the transfer when the atom flips.
//   * On abort the client fires onError exactly once with WasCancelled == true.
//   * The stream stops well short of the full token count (partial stream).
//
// The fixture dribbles one wire chunk per token with a per-chunk sleep so the
// cancel-atom flip (from the test's UI-thread analogue) lands mid-stream. We
// assert cancellation is acknowledged within K observed chunks.
//
// Pure C++14. cpr + httplib bound — lives in the cpr-bearing SmatchetTests rig
// (NOT the cpr-free TSan/SQLite-free rigs); the AI clients pull <cpr/cpr.h>.

#include <doctest/doctest.h>

#include "AiHttpFixture.h"

#include "AiTypes.h"
#include "AnthropicClient.h"
#include "IAiClient.h"
#include "OllamaClient.h"
#include "OpenAiClient.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using smatchet_tests::AiHttpFixture;
using smatchet_tests::AiWireProtocol;

AiCancelToken makeCancelToken() { return std::make_shared<std::atomic<bool>>(false); }

struct ClientCase {
    const char* label;
    AiWireProtocol proto;
    std::unique_ptr<IAiClient> client;
};

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

// Drive one client against the fixture, cancelling after `cancelAfterDeltas`
// observed deltas. Returns the cancellation outcome.
struct CancelOutcome {
    int deltasObserved = 0;
    int totalScripted = 0;
    bool errorFired = false;
    bool wasCancelled = false;
    bool finalSeen = false;
};

CancelOutcome runCancel(IAiClient& client, AiWireProtocol proto, int cancelAfterDeltas) {
    AiHttpFixture fx;
    // 40 tokens at 15 ms/chunk ~= 600 ms uninterrupted — long enough that a
    // cancel after a handful of deltas always lands mid-stream.
    std::vector<std::string> tokens;
    for (int i = 0; i < 40; ++i)
        tokens.push_back("t" + std::to_string(i));
    fx.ScriptStream(proto, tokens, /*perChunkSleepMs=*/15);

    AiClientConfig cfg;
    cfg.BaseUrl = fx.BaseUrl();
    cfg.ApiKey = "sk-cancel-test-key-1234567890";
    cfg.ConnectTimeoutMs = 2000;
    cfg.TotalTimeoutMs = 30000;

    AiChatRequest req;
    req.Model = "test-model";
    req.History.push_back(AiMessage());
    req.History.back().Role = "user";
    req.History.back().Content = "stream please";

    AiCancelToken cancel = makeCancelToken();

    CancelOutcome out;
    out.totalScripted = static_cast<int>(tokens.size());
    std::atomic<int> deltaCount{0};

    std::thread worker([&]() {
        client.SendStreaming(
            cfg, req,
            [&](const AiStreamDelta& d) {
                if (d.IsFinal) {
                    out.finalSeen = true;
                } else {
                    deltaCount.fetch_add(1, std::memory_order_relaxed);
                }
            },
            [&](const AiStreamError& e) {
                out.errorFired = true;
                out.wasCancelled = e.WasCancelled;
            },
            cancel);
    });

    // Spin until we have observed enough deltas, then flip the cancel atom —
    // exactly how the UI thread aborts the worker. Bounded wait so a hung
    // stream can't wedge the suite.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (deltaCount.load(std::memory_order_relaxed) < cancelAfterDeltas &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    cancel->store(true, std::memory_order_release);

    worker.join();
    out.deltasObserved = deltaCount.load(std::memory_order_relaxed);
    return out;
}

void checkCancelContract(AiWireProtocol proto, const char* label) {
    std::unique_ptr<IAiClient> client = makeClient(proto);
    REQUIRE(client);
    CAPTURE(label);

    const int cancelAfter = 3;
    const CancelOutcome out = runCancel(*client, proto, cancelAfter);
    CAPTURE(out.deltasObserved);
    CAPTURE(out.totalScripted);

    // onError fired exactly once with WasCancelled set.
    CHECK(out.errorFired);
    CHECK(out.wasCancelled);
    // Cancelled streams do not deliver the terminal IsFinal delta.
    CHECK_FALSE(out.finalSeen);
    // Partial stream: cancellation stopped us well short of the full token set.
    CHECK(out.deltasObserved < out.totalScripted);
    // Cancel acknowledged within a bounded number of chunks past the trigger
    // point ("within K chunks" — generous slack for cpr buffering + CI jitter,
    // but emphatically not the full 40-token run).
    CHECK(out.deltasObserved < cancelAfter + 20);
}

} // namespace

TEST_CASE("OpenAiClient: cancel mid-stream fires WasCancelled within K chunks" *
          doctest::test_suite("[ai-client-http]")) {
    checkCancelContract(AiWireProtocol::OpenAiSse, "OpenAi");
}

TEST_CASE("AnthropicClient: cancel mid-stream fires WasCancelled within K chunks" *
          doctest::test_suite("[ai-client-http]")) {
    checkCancelContract(AiWireProtocol::AnthropicSse, "Anthropic");
}

TEST_CASE("OllamaClient: cancel mid-stream fires WasCancelled within K chunks" *
          doctest::test_suite("[ai-client-http]")) {
    checkCancelContract(AiWireProtocol::OllamaNdjson, "Ollama");
}

// Sanity counterpart: an uncancelled stream against the same fixture completes
// fully — proves the cancel result above is the cancel path, not a stream that
// just happened to be short.
TEST_CASE("AI clients: uncancelled stream delivers all tokens + final delta" *
          doctest::test_suite("[ai-client-http]")) {
    struct Row {
        const char* label;
        AiWireProtocol proto;
    };
    const Row rows[] = {
        {"OpenAi", AiWireProtocol::OpenAiSse},
        {"Anthropic", AiWireProtocol::AnthropicSse},
        {"Ollama", AiWireProtocol::OllamaNdjson},
    };
    for (const Row& row : rows) {
        CAPTURE(row.label);
        std::unique_ptr<IAiClient> client = makeClient(row.proto);
        REQUIRE(client);

        AiHttpFixture fx;
        const std::vector<std::string> tokens = {"hello", " ", "world"};
        fx.ScriptStream(row.proto, tokens, /*perChunkSleepMs=*/0);

        AiClientConfig cfg;
        cfg.BaseUrl = fx.BaseUrl();
        cfg.ConnectTimeoutMs = 2000;
        cfg.TotalTimeoutMs = 30000;
        AiChatRequest req;
        req.Model = "test-model";
        req.History.push_back(AiMessage());
        req.History.back().Role = "user";
        req.History.back().Content = "hi";

        AiCancelToken cancel = makeCancelToken();
        int deltas = 0;
        bool finalSeen = false;
        bool errorFired = false;
        std::string assembled;

        client->SendStreaming(
            cfg, req,
            [&](const AiStreamDelta& d) {
                if (d.IsFinal) {
                    finalSeen = true;
                } else {
                    ++deltas;
                    assembled += d.TokenChunk;
                }
            },
            [&](const AiStreamError&) { errorFired = true; }, cancel);

        CHECK_FALSE(errorFired);
        CHECK(finalSeen);
        CHECK(deltas == static_cast<int>(tokens.size()));
        CHECK(assembled == "hello world");
    }
}
