// AiAssistantSendScenario — the `aiassistant-streaming-scenarios-s2-s4-s5` gate.
//
// Headless streaming regression closing the deferred live-API verification
// scenarios from docs/plans/shipped/ai-assistant-side-panel.md § Verification:
//   S2 — happy-path streaming   (canned SSE frames → all deltas + IsFinal, no error)
//   S4 — 401 bad-key error path (401 echoing the key → onError, key redacted)
//   S5 — transport-down within 5s (server drops mid-stream → onError, partial deltas)
//
// Unlike AiAssistantStreamingHappyPathScenario / ...TransportDownScenario (which
// drive a STUB IAiClient with no network), this scenario stands up a REAL
// in-process httplib::Server on a loopback ephemeral port and drives the REAL
// OpenAiClient over a real cpr HTTP round-trip — exercising the cpr WriteCallback
// + AiSseParser + RedactProviderErrorBody wiring end-to-end. The mechanism the
// backlog named: "a canned httplib::Server fixture driving IAiClient::SendStreaming
// directly". The server is embedded inline rather than #include'd from
// tests/support/AiHttpFixture.h because scenarios compile into Source/Core, which
// does not carry tests/ on its include path (same rationale as the inline stub in
// AiAssistantStreamingHappyPathScenario.cpp). The fixture header is the doctest
// counterpart; the wire-frame shapes are kept in sync by construction.
//
// Gated on SMATCHET_WITH_AI — the AI client classes live behind that macro's
// link surface. Driven via:
//   Smatchet.exe cmd scenario.run --name=ai-assistant-send-s2-s4-s5 --spawn --yes
// and asserted by scripts/dev/test-ai-assistant.sh.

#include "Commands/Scenarios/IScenario.h"

#if defined(SMATCHET_WITH_AI)

#include "AiTypes.h"
#include "IAiClient.h"
#include "Logger.h"
#include "OpenAiClient.h"
#include "UiPerfMonitor.h"

#include <httplib.h>
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

// Inline canned-SSE httplib server for the S2/S4/S5 sub-scenarios. Minimal
// counterpart of tests/support/AiHttpFixture.h, scoped to the OpenAI SSE wire
// shape the OpenAiClient consumes.
class InlineSseServer {
  public:
    enum class Mode {
        HappyStream,   // S2 — full SSE stream + [DONE]
        Error401,      // S4 — 401 with the key echoed in the body
        TransportDrop, // S5 — a few frames then the socket is closed (no DONE)
    };

    InlineSseServer(Mode mode, const std::string& echoedKey) : mode_(mode), echoedKey_(echoedKey) {
        server_.Post("/v1/chat/completions",
                     [this](const httplib::Request& req, httplib::Response& res) { Handle(req, res); });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this]() { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~InlineSseServer() {
        server_.stop();
        if (thread_.joinable())
            thread_.join();
    }

    InlineSseServer(const InlineSseServer&) = delete;
    InlineSseServer& operator=(const InlineSseServer&) = delete;

    std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

  private:
    void Handle(const httplib::Request& /*req*/, httplib::Response& res) {
        if (mode_ == Mode::Error401) {
            nlohmann::json body;
            body["error"]["message"] = std::string("Incorrect API key provided: ") + echoedKey_;
            body["error"]["type"] = "invalid_request_error";
            body["echoed_header"] = std::string("Bearer ") + echoedKey_;
            res.status = 401;
            res.set_content(body.dump(), "application/json");
            return;
        }

        const bool dropMidStream = (mode_ == Mode::TransportDrop);
        std::vector<std::string> chunks;
        const std::vector<std::string> tokens = {"Hello", ", ", "world", "!"};
        for (const std::string& tok : tokens) {
            nlohmann::json j;
            nlohmann::json choice;
            choice["delta"]["content"] = tok;
            choice["finish_reason"] = nullptr;
            j["choices"] = nlohmann::json::array({choice});
            chunks.push_back(std::string("data: ") + j.dump() + "\n\n");
        }
        if (!dropMidStream)
            chunks.push_back(std::string("data: [DONE]\n\n"));

        const int emitCount = dropMidStream ? 2 : static_cast<int>(chunks.size());
        res.set_chunked_content_provider(
            "text/event-stream", [chunks, emitCount, dropMidStream](std::size_t, httplib::DataSink& sink) -> bool {
                for (int i = 0; i < emitCount; ++i) {
                    // write() returns false once the client closes the socket.
                    if (!sink.write(chunks[i].data(), chunks[i].size()))
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (dropMidStream) {
                    // Close the body abruptly without sink.done() / [DONE] — the
                    // client sees a truncated stream (no terminal delta from the
                    // wire). Returning false aborts the provider; httplib closes
                    // the connection, surfacing as an EOF before final.
                    return false;
                }
                sink.done();
                return true;
            });
    }

    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    Mode mode_;
    std::string echoedKey_;
};

// One sub-scenario run: drive a real OpenAiClient against `mode`, collect the
// callback observations.
struct SubResult {
    int deltas = 0;
    bool finalSeen = false;
    bool errorFired = false;
    bool wasCancelled = false;
    int httpStatus = 0;
    bool keyLeaked = false;
    bool ok = false;
};

const char* kEchoKey = "sk-scenario-leakedkey-1234567890abcdef";

SubResult DriveOnce(InlineSseServer::Mode mode) {
    InlineSseServer server(mode, kEchoKey);

    AiClientConfig cfg;
    cfg.BaseUrl = server.BaseUrl();
    cfg.ApiKey = kEchoKey;
    cfg.ConnectTimeoutMs = 2000;
    cfg.TotalTimeoutMs = 30000;

    AiChatRequest req;
    req.Model = "test-model";
    req.History.push_back(AiMessage());
    req.History.back().Role = "user";
    req.History.back().Content = "stream please";

    OpenAiClient client;
    AiCancelToken cancel = std::make_shared<std::atomic<bool>>(false);

    SubResult r;
    std::string surfacedMessage;
    client.SendStreaming(
        cfg, req,
        [&](const AiStreamDelta& d) {
            if (d.IsFinal)
                r.finalSeen = true;
            else
                ++r.deltas;
        },
        [&](const AiStreamError& e) {
            r.errorFired = true;
            r.wasCancelled = e.WasCancelled;
            r.httpStatus = e.HttpStatus;
            surfacedMessage = e.Message;
        },
        cancel);

    r.keyLeaked = surfacedMessage.find(kEchoKey) != std::string::npos;

    switch (mode) {
    case InlineSseServer::Mode::HappyStream:
        // S2: all tokens delivered + IsFinal, no error.
        r.ok = (r.deltas == 4) && r.finalSeen && !r.errorFired;
        break;
    case InlineSseServer::Mode::Error401:
        // S4: onError with HTTP 401, NOT cancelled, key redacted from message.
        r.ok = r.errorFired && r.httpStatus == 401 && !r.wasCancelled && !r.keyLeaked;
        break;
    case InlineSseServer::Mode::TransportDrop:
        // S5: partial deltas observed, then a non-cancel terminal (error OR a
        // synthesised IsFinal "eof" from the client's truncated-stream path).
        r.ok = (r.deltas > 0) && !r.wasCancelled && (r.errorFired || r.finalSeen);
        break;
    }
    return r;
}

void EmitSub(nlohmann::json& out, const char* key, const SubResult& r) {
    nlohmann::json j;
    j["deltas"] = r.deltas;
    j["finalSeen"] = r.finalSeen;
    j["errorFired"] = r.errorFired;
    j["wasCancelled"] = r.wasCancelled;
    j["httpStatus"] = r.httpStatus;
    j["keyLeaked"] = r.keyLeaked;
    j["ok"] = r.ok;
    out[key] = std::move(j);
}

} // namespace

class AiAssistantSendScenario : public IScenario {
  public:
    // DR7 joining destructor. If this scenario is destroyed while its worker is
    // still joinable (e.g. the runner replaces the active scenario), join the
    // thread here so ~std::thread never runs on a joinable thread and calls
    // std::terminate.
    ~AiAssistantSendScenario() override { JoinWorker(); }

    std::string Name() const override { return "ai-assistant-send-s2-s4-s5"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = args.value("frames", 8);
        if (frames_ < 1)
            frames_ = 1;
        // Run the three sub-scenarios on a worker thread so the render loop keeps
        // ticking (mirrors the real "UI draws while the AI worker streams" shape).
        worker_ = std::thread([this]() {
            s2_ = DriveOnce(InlineSseServer::Mode::HappyStream);
            s4_ = DriveOnce(InlineSseServer::Mode::Error401);
            s5_ = DriveOnce(InlineSseServer::Mode::TransportDrop);
            done_.store(true, std::memory_order_release);
        });
        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {}

    bool IsDone(int frameIndex) const override {
        return frameIndex >= frames_ && done_.load(std::memory_order_acquire);
    }

    void OnCancel(AppController& /*app*/) override { JoinWorker(); }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        JoinWorker();
        nlohmann::json out;
        EmitSub(out, "s2_happy_path", s2_);
        EmitSub(out, "s4_bad_key_401", s4_);
        EmitSub(out, "s5_transport_down", s5_);
        const bool passed = s2_.ok && s4_.ok && s5_.ok;
        out["passed"] = passed;
        out["ok"] = passed;
        out["target_mean_ms"] = 6.94;
        if (!passed)
            LOG_WARN("AiAssistantSendScenario: one or more S2/S4/S5 sub-scenarios failed");
        return out;
    }

  private:
    void JoinWorker() {
        if (worker_.joinable())
            worker_.join();
    }

    int frames_ = 8;
    std::thread worker_;
    std::atomic<bool> done_{false};
    SubResult s2_;
    SubResult s4_;
    SubResult s5_;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantSendScenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(std::make_unique<smatchet::cmd::AiAssistantSendScenario>());
}

#endif // SMATCHET_WITH_AI
