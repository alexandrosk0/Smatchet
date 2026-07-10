// AiAssistantStreamingHappyPathScenario — slice 8 of
// autonomous-debugging-no-creds. Drives the AI streaming SSE surface
// end-to-end via a deterministic stub `IAiClient` (no real HTTP) and
// asserts the UI state transition Idle → InFlight → Idle by counting
// deltas + verifying onError was not fired.
// Why an inline stub rather than #include "tests/support/StubAiClient.h":
// scenarios compile into Source/Core which does not include the tests/
// tree on its include path. The stub shape is duplicated here (kept
// minimal — just `delta_sequence` driving onDelta + a final IsFinal
// delta) so the scenario stays inside Source/Core/.
// The scenario installs the stub via `AiClientFactory::SetTestOverride`,
// fetches a fresh client (which routes through the override), invokes
// SendStreaming on a scenario-owned worker thread, ticks N frames so the
// UI loop continues to render, then joins the worker and asserts the
// transition shape in `rows[]` + the JSON envelope.
// Gated on SMATCHET_WITH_AI — the AI factory + IAiClient interface live
// behind that macro. The DX12 / OFF build registers a no-op stub via the
// `#else` branch so the registry name still resolves.

#include "Commands/Scenarios/IScenario.h"

#if defined(SMATCHET_WITH_AI)

#include "AiClientFactory.h"
#include "AiTypes.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "IAiClient.h"
#include "Logger.h"
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

class StubAiClientStreaming : public IAiClient {
  public:
    explicit StubAiClientStreaming(std::vector<std::string> deltas, int errorAtIndex)
        : deltas_(std::move(deltas)), errorAtIndex_(errorAtIndex) {}

    std::string GetProviderName() const override { return "stub-streaming"; }

    std::string ProbeReachability(const AiClientConfig& /*cfg*/) override { return std::string(); }

    void SendStreaming(const AiClientConfig& /*cfg*/, const AiChatRequest& /*req*/, const DeltaCallback& onDelta,
                       const ErrorCallback& onError, const CancelToken& cancel) override {
        for (std::size_t i = 0; i < deltas_.size(); ++i) {
            if (cancel && cancel->load(std::memory_order_acquire)) {
                if (onError) {
                    AiStreamError err;
                    err.WasCancelled = true;
                    err.Message = "stub: cancelled by token";
                    onError(err);
                }
                return;
            }
            if (errorAtIndex_ >= 0 && static_cast<int>(i) == errorAtIndex_) {
                if (onError) {
                    AiStreamError err;
                    err.HttpStatus = 500;
                    err.Message = "stub: scripted error";
                    onError(err);
                }
                return;
            }
            if (onDelta) {
                AiStreamDelta d;
                d.TokenChunk = deltas_[i];
                d.IsFinal = false;
                onDelta(d);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (onDelta) {
            AiStreamDelta finalDelta;
            finalDelta.IsFinal = true;
            finalDelta.FinishReason = "stop";
            onDelta(finalDelta);
        }
    }

  private:
    std::vector<std::string> deltas_;
    int errorAtIndex_ = -1;
};

// Process-wide pointer the test override reads. Reset to null on scenario
// finish / cancel.
std::vector<std::string>* g_stubDeltas = nullptr;
int g_stubErrorAtIndex = -1;
std::mutex g_stubMutex;

std::unique_ptr<IAiClient> StubFactory(AiProvider /*provider*/) {
    std::lock_guard<std::mutex> lk(g_stubMutex);
    if (!g_stubDeltas) {
        return std::unique_ptr<IAiClient>();
    }
    return std::unique_ptr<IAiClient>(std::make_unique<StubAiClientStreaming>(*g_stubDeltas, g_stubErrorAtIndex));
}

} // namespace

class AiAssistantStreamingHappyPathScenario : public IScenario {
  public:
    // DR7 joining destructor. If this scenario is ever destroyed while its
    // worker is still joinable (e.g. the runner replaces the active scenario),
    // signal the cancel token, join the thread, and clear the factory override
    // here so ~std::thread never runs on a joinable thread (std::terminate) and
    // the stub-backed AiClientFactory override never dangles into freed state.
    ~AiAssistantStreamingHappyPathScenario() override {
        if (cancel_) {
            cancel_->store(true, std::memory_order_release);
        }
        JoinWorker();
        ClearOverride();
    }

    std::string Name() const override { return "ai-assistant-streaming-happy-path"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        frames_ = (std::max)(1, args.value("frames", 60));
        {
            std::lock_guard<std::mutex> lk(g_stubMutex);
            scriptedDeltas_ = {"Hello", ", ", "world", "."};
            g_stubDeltas = &scriptedDeltas_;
            g_stubErrorAtIndex = -1;
        }
        AiClientFactory::SetTestOverride(&StubFactory);
        overrideInstalled_ = true;

        // Build a fresh stub client through the factory — this exercises the
        // SetTestOverride seam end-to-end (the factory consults the override
        // before falling through to the provider switch).
        std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::OpenAi);
        if (!client) {
            ClearOverride();
            outErr = "AiClientFactory::MakeAiClient returned null with test override installed";
            return;
        }

        // Drive the stream on a worker thread; the scenario ticks the UI in
        // parallel so the run reflects the realistic "UI continues drawing
        // while AI worker streams in the background" shape.
        cancel_ = std::make_shared<std::atomic<bool>>(false);
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
                    if (e.WasCancelled) {
                        cancelledFlag_.store(true, std::memory_order_release);
                    }
                },
                cancel_);
            workerDone_.store(true, std::memory_order_release);
        });

        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        // Idle — the worker drives the stream; the UI keeps rendering.
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    void OnCancel(AppController& /*app*/) override {
        if (cancel_) {
            cancel_->store(true, std::memory_order_release);
        }
        JoinWorker();
        ClearOverride();
    }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        JoinWorker();
        ClearOverride();

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
        out["deltasReceived"] = deltasReceived_.load(std::memory_order_acquire);
        out["finalReceived"] = finalReceived_.load(std::memory_order_acquire);
        out["errorReceived"] = errorReceived_.load(std::memory_order_acquire);
        out["cancelled"] = cancelledFlag_.load(std::memory_order_acquire);
        out["expectedDeltas"] = static_cast<int>(scriptedDeltas_.size());
        // Happy-path contract: every scripted delta delivered, final IsFinal
        // delta fired, no error callback.
        const bool ok = (static_cast<std::size_t>(out["deltasReceived"].get<int>()) == scriptedDeltas_.size()) &&
                        finalReceived_.load(std::memory_order_acquire) &&
                        !errorReceived_.load(std::memory_order_acquire);
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
        std::lock_guard<std::mutex> lk(g_stubMutex);
        g_stubDeltas = nullptr;
        g_stubErrorAtIndex = -1;
    }

    int frames_ = 60;
    std::vector<std::string> scriptedDeltas_;
    bool overrideInstalled_ = false;
    std::thread worker_;
    AiCancelToken cancel_;
    std::atomic<int> deltasReceived_{0};
    std::atomic<bool> finalReceived_{false};
    std::atomic<bool> errorReceived_{false};
    std::atomic<bool> cancelledFlag_{false};
    std::atomic<bool> workerDone_{false};
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantStreamingHappyPathScenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(
        std::make_unique<smatchet::cmd::AiAssistantStreamingHappyPathScenario>());
}

#endif // SMATCHET_WITH_AI
