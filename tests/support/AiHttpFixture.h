#ifndef SMATCHET_TESTS_AI_HTTP_FIXTURE_H
#define SMATCHET_TESTS_AI_HTTP_FIXTURE_H

// AiHttpFixture — in-process httplib loopback server for driving the REAL
// production AI clients (OpenAiClient / AnthropicClient / OllamaClient) over a
// real cpr HTTP round-trip, with NO real provider traffic.
//
// Closes the test.md gaps:
//   * `aiclientcancel-per-client-regression` — start a stream, cancel
//     mid-stream, assert WasCancelled within K chunks. Needs a server that
//     dribbles frames slowly enough that the cancel-atom flips before the
//     stream completes; ScriptStreamChunks(... perChunkSleepMs ...) does that.
//   * `per-client-error-body-redaction-gate` — 401 whose body echoes the
//     caller's API key verbatim; assert AiStreamError::Message never contains
//     the literal key (RedactProviderErrorBody must strip it).
//
// Each AI client posts to a fixed path:
//   OpenAi  (OpenAiClient)     POST /v1/chat/completions   (SSE: `data: {...}` + `data: [DONE]`)
//   Anthropic (AnthropicClient) POST /v1/messages          (SSE: `event:`/`data:` frames)
//   Ollama  (OllamaClient)     POST /api/chat              (NDJSON: one JSON object per line)
// The fixture serves the right wire shape per route; the helpers below build
// the canned frames so a test just names the provider + a token list.
//
// httplib's content-provider streaming lets us push chunks with an inter-chunk
// sleep so the cpr WriteCallback (which polls the cancel atom on each chunk)
// gets a real window to observe cancellation. A 401 route returns immediately
// with the echoed-key body.
//
// Pure C++14. Header-only. Links httplib::httplib + nlohmann_json (both already
// on the SmatchetTests link line). Safe to include from any doctest TU.

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet_tests {

/// The three wire dialects the production clients speak. Selects the canned
/// frame builders below + the route the client will POST to.
enum class AiWireProtocol {
    OpenAiSse,    // OpenAiClient — `data: {choices:[{delta:{content}}]}` + `data: [DONE]`
    AnthropicSse, // AnthropicClient — content_block_delta / message_delta / message_stop
    OllamaNdjson, // OllamaClient — `{"message":{"content":...},"done":bool}` per line
};

class AiHttpFixture {
  public:
    AiHttpFixture() {
        // One catch-all POST handler per stream path; the streaming body is set
        // per-request from the scripted chunk plan. Unscripted routes 404.
        server_.Post("/v1/chat/completions",
                     [this](const httplib::Request& req, httplib::Response& res) { Dispatch(req, res); });
        server_.Post("/v1/messages",
                     [this](const httplib::Request& req, httplib::Response& res) { Dispatch(req, res); });
        server_.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) { Dispatch(req, res); });
        port_ = server_.bind_to_any_port("127.0.0.1");
        serverThread_ = std::thread([this]() { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~AiHttpFixture() {
        server_.stop();
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
    }

    AiHttpFixture(const AiHttpFixture&) = delete;
    AiHttpFixture& operator=(const AiHttpFixture&) = delete;

    /// Base URL the client config should point its BaseUrl at.
    std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
    int Port() const { return port_; }

    /// Script a successful streaming response: emit one wire chunk per token
    /// (in the dialect for `proto`), sleeping `perChunkSleepMs` between chunks.
    /// A non-zero sleep gives the cancel-atom poll a real window — the cancel
    /// regression relies on it. The terminal frame (DONE / message_stop /
    /// done:true) is appended automatically.
    void ScriptStream(AiWireProtocol proto, const std::vector<std::string>& tokens, int perChunkSleepMs) {
        std::lock_guard<std::mutex> lk(mutex_);
        proto_ = proto;
        chunks_ = BuildStreamChunks(proto, tokens);
        perChunkSleepMs_ = perChunkSleepMs;
        errorStatus_ = 0;
        errorBody_.clear();
    }

    /// Script a hard HTTP error (e.g. 401) whose body is served verbatim — used
    /// by the redaction gate to echo the caller's API key back. No streaming.
    void ScriptError(int status, const std::string& body) {
        std::lock_guard<std::mutex> lk(mutex_);
        errorStatus_ = status;
        errorBody_ = body;
        chunks_.clear();
    }

    /// Number of times the stream path was hit.
    int RequestCount() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return requestCount_;
    }

    /// Build a 401 body that echoes a provider key verbatim in the shape a real
    /// provider 401 reflects it (so the redactor's per-shape sweeps are exercised).
    static std::string EchoKey401Body(AiWireProtocol proto, const std::string& key) {
        nlohmann::json body;
        switch (proto) {
        case AiWireProtocol::OpenAiSse:
            // OpenAI 401 shape + an echoed Authorization header (misconfigured proxy).
            body["error"]["message"] = std::string("Incorrect API key provided: ") + key;
            body["error"]["type"] = "invalid_request_error";
            body["echoed_header"] = std::string("Bearer ") + key;
            break;
        case AiWireProtocol::AnthropicSse:
            body["type"] = "error";
            body["error"]["type"] = "authentication_error";
            body["error"]["message"] = std::string("invalid x-api-key");
            body["echoed_header"]["x-api-key"] = key;
            break;
        case AiWireProtocol::OllamaNdjson:
            body["error"] = std::string("unauthorized; supplied key: ") + key;
            body["echoed_header"] = std::string("Bearer ") + key;
            break;
        }
        return body.dump();
    }

  private:
    void Dispatch(const httplib::Request& req, httplib::Response& res) {
        AiWireProtocol proto;
        std::vector<std::string> chunks;
        int perChunkSleepMs = 0;
        int errStatus = 0;
        std::string errBody;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ++requestCount_;
            proto = proto_;
            chunks = chunks_;
            perChunkSleepMs = perChunkSleepMs_;
            errStatus = errorStatus_;
            errBody = errorBody_;
        }
        (void)req;

        if (errStatus != 0) {
            res.status = errStatus;
            res.set_content(errBody, "application/json");
            return;
        }

        const char* contentType =
            (proto == AiWireProtocol::OllamaNdjson) ? "application/x-ndjson" : "text/event-stream";
        // Stream the chunks with an inter-chunk sleep via httplib's content
        // provider so the cpr WriteCallback observes them incrementally (the
        // cancel-atom poll runs per delivered chunk).
        res.set_chunked_content_provider(
            contentType, [chunks, perChunkSleepMs](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                for (std::size_t i = 0; i < chunks.size(); ++i) {
                    // sink.write() returns false once the client hangs up (cancel
                    // closed the socket) — stop early so the server thread unwinds.
                    if (!sink.write(chunks[i].data(), chunks[i].size())) {
                        return true;
                    }
                    if (perChunkSleepMs > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(perChunkSleepMs));
                    }
                }
                sink.done();
                return true;
            });
    }

    static std::vector<std::string> BuildStreamChunks(AiWireProtocol proto, const std::vector<std::string>& tokens) {
        std::vector<std::string> out;
        switch (proto) {
        case AiWireProtocol::OpenAiSse: {
            for (const std::string& tok : tokens) {
                nlohmann::json j;
                nlohmann::json choice;
                choice["delta"]["content"] = tok;
                choice["finish_reason"] = nullptr;
                j["choices"] = nlohmann::json::array({choice});
                out.push_back(std::string("data: ") + j.dump() + "\n\n");
            }
            out.push_back(std::string("data: [DONE]\n\n"));
            break;
        }
        case AiWireProtocol::AnthropicSse: {
            for (const std::string& tok : tokens) {
                nlohmann::json j;
                j["type"] = "content_block_delta";
                j["delta"]["type"] = "text_delta";
                j["delta"]["text"] = tok;
                out.push_back(std::string("event: content_block_delta\ndata: ") + j.dump() + "\n\n");
            }
            {
                nlohmann::json md;
                md["type"] = "message_delta";
                md["delta"]["stop_reason"] = "end_turn";
                out.push_back(std::string("event: message_delta\ndata: ") + md.dump() + "\n\n");
            }
            {
                nlohmann::json ms;
                ms["type"] = "message_stop";
                out.push_back(std::string("event: message_stop\ndata: ") + ms.dump() + "\n\n");
            }
            break;
        }
        case AiWireProtocol::OllamaNdjson: {
            for (const std::string& tok : tokens) {
                nlohmann::json j;
                j["message"]["role"] = "assistant";
                j["message"]["content"] = tok;
                j["done"] = false;
                out.push_back(j.dump() + "\n");
            }
            {
                nlohmann::json j;
                j["message"]["role"] = "assistant";
                j["message"]["content"] = "";
                j["done"] = true;
                j["done_reason"] = "stop";
                out.push_back(j.dump() + "\n");
            }
            break;
        }
        }
        return out;
    }

    httplib::Server server_;
    std::thread serverThread_;
    int port_ = 0;

    mutable std::mutex mutex_;
    AiWireProtocol proto_ = AiWireProtocol::OpenAiSse;
    std::vector<std::string> chunks_;
    int perChunkSleepMs_ = 0;
    int errorStatus_ = 0;
    std::string errorBody_;
    int requestCount_ = 0;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_AI_HTTP_FIXTURE_H
