// StubAiClient — header-only `IAiClient` impl for bucket-E tests that need a
// deterministic AI streaming surface without spawning real HTTP traffic.
//
// Wired into production code via `AiClientFactory::SetTestOverride` (see
// `Source/Core/include/AiClientFactory.h`). A test sets the override to a
// thunk that returns a fresh `StubAiClient` per call, runs the code under
// test, then clears the override with `SetTestOverride(nullptr)`.
//
// The stub emits canned SSE deltas via `onDelta` directly — no parser, no
// network, no cpr. `SendStreaming()` is synchronous per the `IAiClient`
// contract ("blocks until... cancelled"), so cancellation has to be flipped
// from another thread (the test spins one). The stub asserts that cancel
// propagates within the configured `cancel_acknowledged_within_ms` budget
// by polling the token between deltas with a small sleep — overshoot
// indicates the contract is broken (caller forgot to flip the bool, or the
// stub script is mis-configured).
//
// Pure C++14 header — no third-party deps beyond <chrono>/<thread> from the
// standard library. Safe to include from any doctest TU.

#ifndef SMATCHET_TESTS_STUB_AI_CLIENT_H
#define SMATCHET_TESTS_STUB_AI_CLIENT_H

#include "AiTypes.h"
#include "IAiClient.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet_tests {

struct StubAiClientScript {
    std::string ProviderName;
    std::vector<std::string> DeltaSequence;
    /// Optional probe error. Empty = ProbeReachability() returns success.
    std::string ProbeError;
    /// Index into DeltaSequence at which to fire `onError` instead of `onDelta`.
    /// -1 = no synthetic error; stream ends with IsFinal delta.
    int ErrorAtIndex = -1;
    /// Synthetic per-delta latency. Higher values make cancellation tests more
    /// deterministic; default keeps the rig fast.
    int PerDeltaSleepMs = 5;
    /// Hard cap on how long the stub will wait for cancellation to propagate
    /// once `cancel` is observed true. If exceeded, the stub trips
    /// `CancelBudgetExceeded` for the test to assert against.
    int CancelAcknowledgedWithinMs = 100;

    StubAiClientScript() = default;
};

class StubAiClient : public IAiClient {
  public:
    explicit StubAiClient(StubAiClientScript script) : script_(std::move(script)) {}

    std::string GetProviderName() const override { return script_.ProviderName; }

    std::string ProbeReachability(const AiClientConfig& /*cfg*/) override { return script_.ProbeError; }

    void SendStreaming(const AiClientConfig& /*cfg*/, const AiChatRequest& /*req*/, const DeltaCallback& onDelta,
                       const ErrorCallback& onError, const CancelToken& cancel) override {
        DeltasEmitted = 0;
        CancelObserved = false;
        CancelBudgetExceeded = false;

        const std::size_t total = script_.DeltaSequence.size();
        for (std::size_t i = 0; i < total; ++i) {
            // Cancel-check between deltas (the IAiClient contract is that cancel is
            // honoured at coalescing boundaries, not mid-callback).
            if (cancel && cancel->load()) {
                CancelObserved = true;
                if (onError) {
                    AiStreamError err;
                    err.WasCancelled = true;
                    err.Message = "stub: cancelled by token";
                    onError(err);
                }
                return;
            }

            if (script_.ErrorAtIndex >= 0 && static_cast<int>(i) == script_.ErrorAtIndex) {
                if (onError) {
                    AiStreamError err;
                    err.HttpStatus = 500;
                    err.Message = "stub: scripted error at index " + std::to_string(i);
                    onError(err);
                }
                return;
            }

            if (onDelta) {
                AiStreamDelta d;
                d.TokenChunk = script_.DeltaSequence[i];
                d.IsFinal = false;
                onDelta(d);
                ++DeltasEmitted;
            }

            // Synthetic per-delta latency, broken into 1 ms ticks so cancel is
            // honoured promptly even when PerDeltaSleepMs is larger than the
            // configured cancel budget.
            //
            // `pollStartedAt` is captured BEFORE entering the polling loop so
            // `ackMs` measures the true poll-to-detect latency (how long the
            // stub kept sleeping after the test flipped the cancel token), not
            // just callback/return overhead. Without this, the budget check is
            // a no-op — the test would stay green even if polling drifted past
            // the advertised budget.
            const auto pollStartedAt = std::chrono::steady_clock::now();
            for (int slept = 0; slept < script_.PerDeltaSleepMs; ++slept) {
                if (cancel && cancel->load()) {
                    CancelObserved = true;
                    if (onError) {
                        AiStreamError err;
                        err.WasCancelled = true;
                        err.Message = "stub: cancelled by token";
                        onError(err);
                    }
                    const auto cancelAckAt = std::chrono::steady_clock::now();
                    const auto ackMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(cancelAckAt - pollStartedAt).count();
                    if (ackMs > script_.CancelAcknowledgedWithinMs)
                        CancelBudgetExceeded = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // Stream completed without error or cancel — emit final delta.
        if (onDelta) {
            AiStreamDelta finalDelta;
            finalDelta.IsFinal = true;
            finalDelta.FinishReason = "stop";
            onDelta(finalDelta);
        }
    }

    // Observability surface for tests — written exclusively from the stub's
    // own SendStreaming() so race-free reads require a join on the worker.
    std::size_t DeltasEmitted = 0;
    bool CancelObserved = false;
    bool CancelBudgetExceeded = false;

  private:
    StubAiClientScript script_;
};

} // namespace smatchet_tests

#endif
