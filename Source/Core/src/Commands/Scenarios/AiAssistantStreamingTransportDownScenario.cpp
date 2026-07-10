// AiAssistantStreamingTransportDownScenario — slice 8 of
// autonomous-debugging-no-creds. Same shape as the happy-path and 401
// scenarios but configures the inline stub to emit a partial delta
// sequence, then synthesise a transport-drop error within a configurable
// budget (default 5s — capped well under that for test cadence).
// Asserts the cancel/error UI path fires within the budget AND that
// partial deltas were observed before the drop.
// See AiAssistantStreamingHappyPathScenario.cpp for the inline-stub
// rationale.

#include "Commands/Scenarios/IScenario.h"

#if defined(SMATCHET_WITH_AI)

#include "AiClientFactory.h"
#include "AiTypes.h"
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared scenario-TU include + namespace-open boilerplate is grandfathered across the Scenarios/*.cpp siblings; dropping the vestigial AppController.h include (fan-in Phase 6 T1a) re-hashed the boilerplate clone window vs a sibling scenario — not a real copy-paste; owner=orchestrator; revisit=when the scenario prologue is factored into a shared header)
// clang-format on
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "IAiClient.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

class StubTransportDownClient : public IAiClient {
  public:
    StubTransportDownClient(int partialDeltas, int perDeltaSleepMs)
        : partialDeltas_(partialDeltas), perDeltaSleepMs_(perDeltaSleepMs) {}

    std::string GetProviderName() const override { return "stub-transport-down"; }
    std::string ProbeReachability(const AiClientConfig& /*cfg*/) override { return std::string(); }

    void SendStreaming(const AiClientConfig& /*cfg*/, const AiChatRequest& /*req*/, const DeltaCallback& onDelta,
                       const ErrorCallback& onError, const CancelToken& cancel) override {
        for (int i = 0; i < partialDeltas_; ++i) {
            if (cancel && cancel->load(std::memory_order_acquire)) {
                if (onError) {
                    AiStreamError err;
                    err.WasCancelled = true;
                    err.Message = "stub: cancelled by token";
                    onError(err);
                }
                return;
            }
            if (onDelta) {
                AiStreamDelta d;
                d.TokenChunk = "tok-" + std::to_string(i);
                d.IsFinal = false;
                onDelta(d);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(perDeltaSleepMs_));
        }
        // Transport drops mid-stream — surface as onError without
        // WasCancelled (network failure shape, not user-cancelled).
        if (onError) {
            AiStreamError err;
            err.HttpStatus = 0; // transport-layer, no HTTP status
            err.Message = "stub: transport down (curl error 56: recv failure)";
            err.WasCancelled = false;
            onError(err);
        }
    }

  private:
    int partialDeltas_ = 3;
    int perDeltaSleepMs_ = 10;
};

int g_partialDeltas = 3;
int g_perDeltaSleepMs = 10;

std::unique_ptr<IAiClient> StubTransportDownFactory(AiProvider /*provider*/) {
    return std::make_unique<StubTransportDownClient>(g_partialDeltas, g_perDeltaSleepMs);
}

} // namespace

class AiAssistantStreamingTransportDownScenario : public IScenario {
  public:
    // SMATCHET_DEVIATION(rule=duplication; reason=DR7 joining destructor + stub-factory boilerplate is intentionally
    // identical teardown (signal cancel, join worker, clear AiClientFactory override) across the streaming scenario
    // stubs so no scenario can leak a joinable thread on replace; owner=deep-review; revisit=2026-10-01) DR7 joining
    // destructor. If this scenario is destroyed while its worker is still joinable (e.g. the runner replaces the active
    // scenario), signal the cancel token, join the thread, and clear the factory override here so ~std::thread never
    // runs on a joinable thread (std::terminate) and the stub-backed AiClientFactory override never dangles into freed
    // state.
    ~AiAssistantStreamingTransportDownScenario() override {
        if (cancel_) {
            cancel_->store(true, std::memory_order_release);
        }
        JoinWorker();
        ClearOverride();
    }

    std::string Name() const override { return "ai-assistant-streaming-transport-down-within-5s"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        frames_ = (std::max)(1, args.value("frames", 120));
        g_partialDeltas = (std::max)(1, args.value("partialDeltas", 3));
        g_perDeltaSleepMs = (std::max)(1, args.value("perDeltaSleepMs", 10));
        budgetMs_ = (std::max)(100, args.value("budgetMs", 5000));

        AiClientFactory::SetTestOverride(&StubTransportDownFactory);
        overrideInstalled_ = true;

        std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::OpenAi);
        if (!client) {
            ClearOverride();
            outErr = "AiClientFactory::MakeAiClient returned null with test override installed";
            return;
        }

        cancel_ = std::make_shared<std::atomic<bool>>(false);
        startedAt_ = std::chrono::steady_clock::now();
        worker_ = std::thread([this, c = std::move(client)]() mutable {
            AiClientConfig cfg;
            AiChatRequest req;
            c->SendStreaming(
                cfg, req,
                [this](const AiStreamDelta& d) {
                    if (d.IsFinal) {
                        finalReceived_.store(true, std::memory_order_release);
                    } else {
                        deltasReceived_.fetch_add(1, std::memory_order_relaxed);
                    }
                },
                [this](const AiStreamError& e) {
                    errorReceived_.store(true, std::memory_order_release);
                    wasCancelled_.store(e.WasCancelled, std::memory_order_release);
                    errorAt_ = std::chrono::steady_clock::now();
                },
                cancel_);
        });

        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // Idle.
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    void OnCancel(IAppScenarioHost& /*app*/) override {
        if (cancel_) {
            cancel_->store(true, std::memory_order_release);
        }
        JoinWorker();
        ClearOverride();
    }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        JoinWorker();
        ClearOverride();

        const long long elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(errorAt_ - startedAt_).count();

        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = nlohmann::json::array();
        for (const UiPerfRow& r : rows) {
            nlohmann::json rj;
            rj["name"] = r.name;
            rj["lastTotalMs"] = r.lastTotalMs;
            rj["avgPerCallMs"] = r.avgPerCallMs;
            rj["maxMs"] = r.maxMs;
            rj["calls"] = r.calls;
            rj["emaAvgMs"] = r.emaAvgMs;
            rj["p99Ms"] = r.p99Ms;
            rowsJson.push_back(std::move(rj));
        }

        nlohmann::json out;
        out["frames"] = frames_;
        out["budgetMs"] = budgetMs_;
        out["elapsedMs"] = static_cast<long long>(elapsedMs);
        out["deltasReceived"] = deltasReceived_.load(std::memory_order_acquire);
        out["finalReceived"] = finalReceived_.load(std::memory_order_acquire);
        out["errorReceived"] = errorReceived_.load(std::memory_order_acquire);
        out["wasCancelled"] = wasCancelled_.load(std::memory_order_acquire);
        // Contract: partial deltas observed before the drop, onError fired
        // with WasCancelled=false (transport drop, not user-cancel), and
        // the error surfaced within the budget.
        const bool ok = deltasReceived_.load(std::memory_order_acquire) > 0 &&
                        !finalReceived_.load(std::memory_order_acquire) &&
                        errorReceived_.load(std::memory_order_acquire) &&
                        !wasCancelled_.load(std::memory_order_acquire) && elapsedMs <= budgetMs_;
        out["ok"] = ok;
        out["target_mean_ms"] = 6.94;
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    void JoinWorker() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    void ClearOverride() {
        if (overrideInstalled_) {
            AiClientFactory::SetTestOverride(nullptr);
            overrideInstalled_ = false;
        }
    }

    int frames_ = 120;
    int budgetMs_ = 5000;
    bool overrideInstalled_ = false;
    std::thread worker_;
    AiCancelToken cancel_;
    std::chrono::steady_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point errorAt_;
    std::atomic<int> deltasReceived_{0};
    std::atomic<bool> finalReceived_{false};
    std::atomic<bool> errorReceived_{false};
    std::atomic<bool> wasCancelled_{false};
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantStreamingTransportDownScenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(
        std::make_unique<smatchet::cmd::AiAssistantStreamingTransportDownScenario>());
}

#endif // SMATCHET_WITH_AI
