// StubAiClient cancellation contract — verifies the test-infrastructure stub
// (tests/support/StubAiClient.h) honours its AiCancelToken contract within
// the configured budget. NOT a test of any production AI client; bucket-E
// tests inject this stub via `AiClientFactory::SetTestOverride`, so the
// stub's own cancel-propagation behaviour is the load-bearing contract.
//
// Pillar 2 (UI never freezes) is enforced upstream by the AssistantViewModel
// worker thread joining on cancel; this test guarantees the stub used to
// exercise that path stops streaming once cancelled. The cancel-ack latency
// budgets are advisory (doctest WARN_*): wall-clock time is not deterministic
// on a loaded CI runner, so an overrun is reported but never fails the case.
// The cancel behaviour itself is pinned by a case that cancels from inside
// onDelta, which needs no clock at all.
//
// Pure C++14 — std::thread + std::chrono + AiCancelToken (= shared_ptr<atomic<bool>>).
// No HTTP, no cpr, no production AI client.

#include "AiTypes.h"
#include "StubAiClient.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

AiCancelToken makeCancelToken() { return std::make_shared<std::atomic<bool>>(false); }

} // namespace

TEST_CASE("StubAiClient: cancel mid-stream stops onDelta within 100 ms") {
    smatchet_tests::StubAiClientScript script;
    script.ProviderName = "stub-cancel";
    // Long-ish stream — at 5 ms per delta plus our forced 5 ms sleep budget
    // inside the stub, the full stream takes ~500 ms uninterrupted. We
    // cancel after 50 ms and expect to see far fewer than 100 deltas.
    script.DeltaSequence.assign(100, std::string("tok"));
    script.PerDeltaSleepMs = 5;
    script.CancelAcknowledgedWithinMs = 100;

    smatchet_tests::StubAiClient stub(script);
    AiCancelToken cancel = makeCancelToken();

    std::vector<AiStreamDelta> deltas;
    AiStreamError finalError;
    bool errorFired = false;

    const auto startedAt = std::chrono::steady_clock::now();

    // Worker thread mirrors AssistantViewModel's dispatcher: SendStreaming()
    // blocks; the UI thread is what flips the cancel bool.
    std::thread worker([&]() {
        AiClientConfig cfg;
        AiChatRequest req;
        stub.SendStreaming(
            cfg, req, [&](const AiStreamDelta& d) { deltas.push_back(d); },
            [&](const AiStreamError& e) {
                finalError = e;
                errorFired = true;
            },
            cancel);
    });

    // Let a handful of deltas land, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto cancelSetAt = std::chrono::steady_clock::now();
    cancel->store(true);

    worker.join();
    const auto returnedAt = std::chrono::steady_clock::now();

    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(returnedAt - startedAt).count();
    const auto postCancelMs = std::chrono::duration_cast<std::chrono::milliseconds>(returnedAt - cancelSetAt).count();

    // Cancel ack within the configured budget (100 ms), not the full 500 ms
    // uninterrupted run. Advisory only (WARN_*): a contended runner (a cold full
    // compile beside ctest) has overrun it on token-identical code, so a budget
    // miss is reported without failing a required check (backlog test entry
    // 2026-08-17-stubaiclient-cancel-100ms-budget-is-flaky-on-ci).
#if defined(__SANITIZE_ADDRESS__)
    WARN_LT(postCancelMs, 2000); // ASAN ~3-10x wall-clock overhead; budget loosened (#1215 pattern)
    WARN_LT(totalMs, 4000);
#elif defined(SMATCHET_COVERAGE)
    // OpenCppCoverage (coverage.yml) runs the binary instrumented (~10x slower)
    // and defines NO __SANITIZE_ADDRESS__. Scale x8 — same ratio the
    // CallstackParser ReDoS-timing guard uses for SMATCHET_COVERAGE, and matching
    // the stub's internal ack budget (#1280).
    WARN_LT(postCancelMs, 1600);
    WARN_LT(totalMs, 3200);
#else
    WARN_LT(postCancelMs, 200);
    WARN_LT(totalMs, 400);
#endif
    WARN_FALSE(stub.CancelBudgetExceeded);

    // The cancel lands mid-stream only if this thread woke from its 50 ms sleep before the ~500 ms
    // stream ended, which a stalled runner cannot promise, so these are advisory too. The case below
    // pins the same behaviour deterministically.
    WARN(stub.CancelObserved);
    WARN(errorFired);
    WARN(finalError.WasCancelled);
    WARN(deltas.size() < 100u);
    WARN(std::none_of(deltas.begin(), deltas.end(), [](const AiStreamDelta& d) { return d.IsFinal; }));
    CHECK(deltas.size() == stub.DeltasEmitted); // every delta the stub emitted reached the callback
}

TEST_CASE("StubAiClient: a cancel observed between deltas ends the stream with the cancel error") {
    // Deterministic: the cancel is set from inside onDelta (on the streaming thread) after the 5th
    // delta, and the stub checks the token before each delta, so exactly 5 deltas are delivered.
    smatchet_tests::StubAiClientScript script;
    script.ProviderName = "stub-cancel-deterministic";
    script.DeltaSequence.assign(100, std::string("tok"));
    script.PerDeltaSleepMs = 0;

    smatchet_tests::StubAiClient stub(script);
    AiCancelToken cancel = makeCancelToken();
    std::vector<AiStreamDelta> deltas;
    AiStreamError finalError;
    bool errorFired = false;

    AiClientConfig cfg;
    AiChatRequest req;
    stub.SendStreaming(
        cfg, req,
        [&](const AiStreamDelta& d) {
            deltas.push_back(d);
            if (deltas.size() == 5u) {
                cancel->store(true);
            }
        },
        [&](const AiStreamError& e) {
            finalError = e;
            errorFired = true;
        },
        cancel);

    CHECK(stub.CancelObserved);
    CHECK(errorFired);
    CHECK(finalError.WasCancelled);
    CHECK(deltas.size() == 5u);
    CHECK(deltas.size() == stub.DeltasEmitted);
    CHECK(std::none_of(deltas.begin(), deltas.end(), [](const AiStreamDelta& d) { return d.IsFinal; }));
}

TEST_CASE("StubAiClient: completes full stream when never cancelled and emits final IsFinal delta") {
    smatchet_tests::StubAiClientScript script;
    script.ProviderName = "stub-happy";
    script.DeltaSequence = {"hello", " ", "world"};
    script.PerDeltaSleepMs = 1;

    smatchet_tests::StubAiClient stub(script);
    AiCancelToken cancel = makeCancelToken();

    std::vector<AiStreamDelta> deltas;
    bool errorFired = false;

    AiClientConfig cfg;
    AiChatRequest req;
    stub.SendStreaming(
        cfg, req, [&](const AiStreamDelta& d) { deltas.push_back(d); },
        [&](const AiStreamError&) { errorFired = true; }, cancel);

    REQUIRE(deltas.size() == 4u); // 3 token deltas + 1 final
    CHECK(deltas[0].TokenChunk == "hello");
    CHECK(deltas[1].TokenChunk == " ");
    CHECK(deltas[2].TokenChunk == "world");
    CHECK_FALSE(deltas[0].IsFinal);
    CHECK(deltas[3].IsFinal);
    CHECK(deltas[3].FinishReason == "stop");
    CHECK_FALSE(errorFired);
    CHECK_FALSE(stub.CancelObserved);
}

TEST_CASE("StubAiClient: scripted error fires at configured index and stops stream") {
    smatchet_tests::StubAiClientScript script;
    script.ProviderName = "stub-error";
    script.DeltaSequence = {"a", "b", "c", "d"};
    script.ErrorAtIndex = 2;
    script.PerDeltaSleepMs = 0;

    smatchet_tests::StubAiClient stub(script);
    AiCancelToken cancel = makeCancelToken();

    std::vector<AiStreamDelta> deltas;
    AiStreamError observed;
    bool errorFired = false;

    AiClientConfig cfg;
    AiChatRequest req;
    stub.SendStreaming(
        cfg, req, [&](const AiStreamDelta& d) { deltas.push_back(d); },
        [&](const AiStreamError& e) {
            observed = e;
            errorFired = true;
        },
        cancel);

    REQUIRE(deltas.size() == 2u); // indices 0 and 1, then error at 2
    CHECK(deltas[0].TokenChunk == "a");
    CHECK(deltas[1].TokenChunk == "b");
    CHECK(errorFired);
    CHECK(observed.HttpStatus == 500);
    CHECK_FALSE(observed.WasCancelled);
}

TEST_CASE("StubAiClient: ProbeReachability returns configured error string") {
    smatchet_tests::StubAiClientScript script;
    script.ProviderName = "stub-probe";
    script.ProbeError = "stub: unreachable";

    smatchet_tests::StubAiClient stub(script);
    AiClientConfig cfg;
    CHECK(stub.ProbeReachability(cfg) == "stub: unreachable");
    CHECK(stub.GetProviderName() == "stub-probe");
}

TEST_CASE("StubAiClient: empty delta sequence emits only the final delta") {
    smatchet_tests::StubAiClientScript script;
    script.ProviderName = "stub-empty";
    // No deltas — single IsFinal sentinel only.

    smatchet_tests::StubAiClient stub(script);
    AiCancelToken cancel = makeCancelToken();
    std::vector<AiStreamDelta> deltas;

    AiClientConfig cfg;
    AiChatRequest req;
    stub.SendStreaming(
        cfg, req, [&](const AiStreamDelta& d) { deltas.push_back(d); }, [](const AiStreamError&) {}, cancel);

    REQUIRE(deltas.size() == 1u);
    CHECK(deltas[0].IsFinal);
    CHECK(deltas[0].TokenChunk.empty());
}
