#include "AnthropicClient.h"

#include "AiErrorRedact.h"
#include "AiSseParser.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <string>

namespace {

constexpr const char* kDefaultBaseUrl = "https://api.anthropic.com";
constexpr const char* kAnthropicApiVersion = "2023-06-01";
constexpr int kDefaultMaxTokens = 4096;

std::string ResolveBaseUrl(const AiClientConfig& cfg) {
    if (!cfg.BaseUrl.empty())
        return cfg.BaseUrl;
    return kDefaultBaseUrl;
}

std::string JoinUrl(const std::string& base, const char* path) {
    if (base.empty())
        return std::string(path);
    if (base.back() == '/')
        return base.substr(0, base.size() - 1) + path;
    return base + path;
}

nlohmann::json BuildChatBody(const AiChatRequest& req) {
    nlohmann::json body;
    body["model"] = req.Model;
    body["stream"] = true;
    body["max_tokens"] = (req.MaxTokens > 0) ? req.MaxTokens : kDefaultMaxTokens;
    if (req.Temperature >= 0.0f)
        body["temperature"] = req.Temperature;
    if (!req.SystemPrompt.empty())
        body["system"] = req.SystemPrompt;

    // Anthropic Messages API: `messages` is user/assistant only — system text lives at top level.
    // Roles are passed through as-is; AppController is responsible for emitting "user" / "assistant".
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& h : req.History) {
        nlohmann::json m;
        m["role"] = h.Role;
        m["content"] = h.Content;
        messages.push_back(std::move(m));
    }
    body["messages"] = std::move(messages);
    return body;
}

// Translate one decoded SSE event into at most one AiStreamDelta. Mutates
// sawFinal when the stream terminates so the WriteCallback can short-circuit.
void DispatchAnthropicEvent(const AiSseParser::Event& ev, const IAiClient::DeltaCallback& onDelta, bool& sawFinal,
                            std::string& pendingFinishReason) {
    // We ignore: message_start, content_block_start, content_block_stop, ping, error.
    // We consume: content_block_delta (token chunk), message_delta (stop_reason), message_stop (terminal).
    if (ev.Name != "content_block_delta" && ev.Name != "message_delta" && ev.Name != "message_stop")
        return;

    if (ev.Data.empty())
        return;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ev.Data);
    } catch (const std::exception& e) {
        LOG_WARN("AnthropicClient: SSE data not valid JSON (%s): %.*s", e.what(),
                 static_cast<int>(ev.Data.size() > 200 ? 200 : ev.Data.size()), ev.Data.c_str());
        return;
    }
    if (!j.is_object())
        return;

    if (ev.Name == "content_block_delta") {
        if (!j.contains("delta") || !j["delta"].is_object())
            return;
        const auto& delta = j["delta"];
        if (!delta.contains("text") || !delta["text"].is_string())
            return;
        AiStreamDelta d;
        d.TokenChunk = delta["text"].get<std::string>();
        d.IsFinal = false;
        if (!d.TokenChunk.empty())
            onDelta(d);
        return;
    }

    if (ev.Name == "message_delta") {
        // Anthropic sends stop_reason on the `message_delta` event ahead of `message_stop`.
        // Cache it so we can surface it on the terminal delta.
        if (j.contains("delta") && j["delta"].is_object()) {
            const auto& delta = j["delta"];
            if (delta.contains("stop_reason") && delta["stop_reason"].is_string())
                pendingFinishReason = delta["stop_reason"].get<std::string>();
        }
        return;
    }

    // ev.Name == "message_stop"
    AiStreamDelta d;
    d.IsFinal = true;
    d.FinishReason = pendingFinishReason;
    onDelta(d);
    sawFinal = true;
}

} // namespace

std::string AnthropicClient::GetProviderName() const { return "anthropic"; }

std::string AnthropicClient::ProbeReachability(const AiClientConfig& cfg) {
    // Anthropic has no public unauthenticated probe endpoint. Issue a HEAD against
    // /v1/messages — a 4xx with an Anthropic-shaped error body proves reachability,
    // while transport errors / 5xx are real outages.
    const std::string url = JoinUrl(ResolveBaseUrl(cfg), "/v1/messages");
    cpr::Header headers{
        {"Accept", "application/json"},
        {"anthropic-version", kAnthropicApiVersion},
    };
    if (!cfg.ApiKey.empty())
        headers["x-api-key"] = cfg.ApiKey;

    cpr::Response r =
        cpr::Head(cpr::Url{url}, headers, cpr::ConnectTimeout{cfg.ConnectTimeoutMs}, cpr::Timeout{cfg.TotalTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Ai, NetworkUsageTracker::kEstimatedGetUploadBytes, r);
    if (r.error.code != cpr::ErrorCode::OK)
        return std::string("transport: ") + r.error.message;
    // 4xx (e.g. 401 missing key, 405 method not allowed) is fine — host is reachable.
    if (r.status_code >= 500)
        return std::string("HTTP ") + std::to_string(r.status_code);
    return std::string();
}

void AnthropicClient::SendStreaming(const AiClientConfig& cfg, const AiChatRequest& req, const DeltaCallback& onDelta,
                                    const ErrorCallback& onError, const CancelToken& cancel) {
    const std::string url = JoinUrl(ResolveBaseUrl(cfg), "/v1/messages");
    const std::string body = BuildChatBody(req).dump();

    cpr::Header headers{
        {"Accept", "text/event-stream"},
        {"Content-Type", "application/json"},
        {"Cache-Control", "no-cache"},
        {"anthropic-version", kAnthropicApiVersion},
    };
    if (!cfg.ApiKey.empty())
        headers["x-api-key"] = cfg.ApiKey;

    AiSseParser parser;
    bool sawFinal = false;
    bool cancelObserved = false;
    std::string pendingFinishReason;

    auto translate = [&](const AiSseParser::Event& ev) {
        if (cancelObserved || sawFinal)
            return;
        DispatchAnthropicEvent(ev, onDelta, sawFinal, pendingFinishReason);
    };

    cpr::WriteCallback wcb{[&](const std::string& chunk, intptr_t) -> bool {
                               if (cancel && cancel->load(std::memory_order_relaxed)) {
                                   cancelObserved = true;
                                   return false;
                               }
                               parser.Feed(chunk.data(), chunk.size(), translate);
                               if (cancel && cancel->load(std::memory_order_relaxed)) {
                                   cancelObserved = true;
                                   return false;
                               }
                               return !sawFinal;
                           },
                           0};

    cpr::Response r = cpr::Post(cpr::Url{url}, headers, cpr::Body{body}, wcb, cpr::ConnectTimeout{cfg.ConnectTimeoutMs},
                                cpr::Timeout{cfg.TotalTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Ai, static_cast<std::uint64_t>(body.size()), r);

    if (!sawFinal)
        parser.Flush(translate);

    if (cancelObserved) {
        AiStreamError err;
        err.WasCancelled = true;
        err.Message = "Cancelled by user";
        onError(err);
        return;
    }

    if (r.error.code != cpr::ErrorCode::OK && r.error.code != cpr::ErrorCode::REQUEST_CANCELLED) {
        LOG_ERROR("AnthropicClient: transport error - cpr code %d, message: %s",
                  static_cast<int>(r.error.code), r.error.message.c_str());
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = std::string("transport: ") + r.error.message;
        onError(err);
        return;
    }

    if (r.status_code < 200 || r.status_code >= 300) {
        // Provider HTTP error visibility: 4xx/5xx previously flowed silently
        // to the UI's assistantLastError strip without ever touching the
        // Logger. LOG_ERROR includes a redacted body excerpt (sanitised by
        // AiErrorRedact - strips api keys / org ids / Authorization echoes
        // plus length-cap) so users can diagnose without leaking secrets.
        const std::string redactedBody = smatchet::ai::pure::RedactProviderErrorBody(r.text);
        LOG_ERROR("AnthropicClient: HTTP %ld - body: %s", static_cast<long>(r.status_code), redactedBody.c_str());
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = std::string("HTTP ") + std::to_string(r.status_code);
        if (!redactedBody.empty()) {
            err.Message.append(": ");
<<<<<<< HEAD
            err.Message.append(redactedBody);
=======
            err.Message.append(smatchet::ai::pure::RedactProviderErrorBody(r.text));
>>>>>>> 19fc262 (fix(ai-hotfix-batch-2): four CRITICAL + eight HIGH findings from retrospective review)
        }
        onError(err);
        return;
    }

    if (!sawFinal) {
        AiStreamDelta d;
        d.IsFinal = true;
        d.FinishReason = "eof";
        onDelta(d);
    }
}
