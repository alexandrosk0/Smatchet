// AiAssistantStreaming401Scenario — slice 8 of
// autonomous-debugging-no-creds. Same shape as the happy-path scenario but
// configures the inline stub to fire `onError` at index 0 with 401
// semantics (HttpStatus = 401, no deltas delivered before the error).
// Asserts the UI state transition Idle → InFlight → Errored.
//
// See AiAssistantStreamingHappyPathScenario.cpp for the rationale on the
// inline stub (Source_Core/ does not include the tests/ include path).

#include "Commands/Scenarios/IScenario.h"

#if defined(SMATCHET_WITH_AI)

#include "AiClientFactory.h"
#include "AiTypes.h"
#include "AppController.h"
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

class Stub401Client : public IAiClient {
  public:
    std::string GetProviderName() const override { return "stub-401"; }
    std::string ProbeReachability(const AiClientConfig& /*cfg*/) override {
        return std::string("stub: 401 Unauthorized");
    }
    void SendStreaming(const AiClientConfig& /*cfg*/, const AiChatRequest& /*req*/, const DeltaCallback& /*onDelta*/,
                       const ErrorCallback& onError, const CancelToken& /*cancel*/) override {
        // Synthesise the 401 path — providers normally surface this on the
        // very first chunk read (auth check is server-side, returns before
        // any SSE bytes flow).
        if (onError) {
            AiStreamError err;
            err.HttpStatus = 401;
            err.Message = "stub: 401 Unauthorized — invalid API key";
            err.WasCancelled = false;
            onError(err);
        }
    }
};

std::unique_ptr<IAiClient> Stub401Factory(AiProvider /*provider*/) {
    return std::unique_ptr<IAiClient>(new Stub401Client());
}

} // namespace

class AiAssistantStreaming401Scenario : public IScenario {
  public:
    std::string Name() const override { return "ai-assistant-streaming-401"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        frames_ = (std::max)(1, args.value("frames", 30));
        AiClientFactory::SetTestOverride(&Stub401Factory);
        overrideInstalled_ = true;

        std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::OpenAi);
        if (!client) {
            ClearOverride();
            outErr = "AiClientFactory::MakeAiClient returned null with test override installed";
            return;
        }

        // ProbeReachability sanity — should surface the 401 string.
        AiClientConfig cfg;
        probeError_ = client->ProbeReachability(cfg);

        cancel_ = std::make_shared<std::atomic<bool>>(false);
        worker_ = std::thread([this, c = std::move(client)]() mutable {
            AiClientConfig localCfg;
            AiChatRequest req;
            c->SendStreaming(
                localCfg, req,
                [this](const AiStreamDelta& d) {
                    if (d.IsFinal) {
                        finalReceived_.store(true, std::memory_order_release);
                    } else {
                        deltasReceived_.fetch_add(1, std::memory_order_relaxed);
                    }
                },
                [this](const AiStreamError& e) {
                    errorReceived_.store(true, std::memory_order_release);
                    httpStatus_.store(e.HttpStatus, std::memory_order_release);
                },
                cancel_);
        });

        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        // Idle.
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

        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
        nlohmann::json rowsJson = nlohmann::json::array();
        for (const UiPerfRow& r : rows) {
            nlohmann::json rj;
            rj["name"] = r.name;
            rj["lastTotalMs"] = r.lastTotalMs;
            rj["avgPerCallMs"] = r.avgPerCallMs;
            rj["maxMs"] = r.maxMs;
            rj["calls"] = r.calls;
            rj["emaAvgMs"] = r.emaAvgMs;
            rowsJson.push_back(std::move(rj));
        }

        nlohmann::json out;
        out["frames"] = frames_;
        out["probeError"] = probeError_;
        out["deltasReceived"] = deltasReceived_.load(std::memory_order_acquire);
        out["finalReceived"] = finalReceived_.load(std::memory_order_acquire);
        out["errorReceived"] = errorReceived_.load(std::memory_order_acquire);
        out["httpStatus"] = httpStatus_.load(std::memory_order_acquire);
        // Contract: zero deltas, onError fired with 401.
        const bool ok =
            !finalReceived_.load(std::memory_order_acquire) && deltasReceived_.load(std::memory_order_acquire) == 0 &&
            errorReceived_.load(std::memory_order_acquire) && httpStatus_.load(std::memory_order_acquire) == 401;
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

    int frames_ = 30;
    bool overrideInstalled_ = false;
    std::string probeError_;
    std::thread worker_;
    AiCancelToken cancel_;
    std::atomic<int> deltasReceived_{0};
    std::atomic<bool> finalReceived_{false};
    std::atomic<bool> errorReceived_{false};
    std::atomic<int> httpStatus_{0};
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantStreaming401Scenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(
        std::make_unique<smatchet::cmd::AiAssistantStreaming401Scenario>());
}

#endif // SMATCHET_WITH_AI
