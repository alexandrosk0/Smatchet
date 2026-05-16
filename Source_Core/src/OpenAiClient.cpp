#include "OpenAiClient.h"

#include "AiSseParser.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace {

constexpr const char* kDefaultBaseUrl = "https://api.openai.com";

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
    if (req.Temperature >= 0.0f)
        body["temperature"] = req.Temperature;
    if (req.MaxTokens > 0)
        body["max_tokens"] = req.MaxTokens;

    nlohmann::json messages = nlohmann::json::array();
    if (!req.SystemPrompt.empty()) {
        nlohmann::json m;
        m["role"] = "system";
        m["content"] = req.SystemPrompt;
        messages.push_back(std::move(m));
    }
    for (const auto& h : req.History) {
        nlohmann::json m;
        m["role"] = h.Role;
        m["content"] = h.Content;
        messages.push_back(std::move(m));
    }
    body["messages"] = std::move(messages);
    return body;
}

void DispatchOpenAiDataLine(const std::string& data, const IAiClient::DeltaCallback& onDelta, bool& sawFinal) {
    if (data == "[DONE]") {
        AiStreamDelta d;
        d.IsFinal = true;
        d.FinishReason = "stop";
        onDelta(d);
        sawFinal = true;
        return;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(data);
    } catch (const std::exception& e) {
        LOG_WARN("OpenAiClient: SSE data not valid JSON (%s): %.*s", e.what(),
                 static_cast<int>(data.size() > 200 ? 200 : data.size()), data.c_str());
        return;
    }
    if (!j.is_object() || !j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
        return;
    const auto& ch0 = j["choices"][0];
    AiStreamDelta d;
    if (ch0.contains("delta") && ch0["delta"].is_object()) {
        const auto& delta = ch0["delta"];
        if (delta.contains("content") && delta["content"].is_string())
            d.TokenChunk = delta["content"].get<std::string>();
    }
    if (ch0.contains("finish_reason") && ch0["finish_reason"].is_string()) {
        d.FinishReason = ch0["finish_reason"].get<std::string>();
        d.IsFinal = !d.FinishReason.empty();
        if (d.IsFinal)
            sawFinal = true;
    }
    if (!d.TokenChunk.empty() || d.IsFinal)
        onDelta(d);
}

} // namespace

std::string OpenAiClient::GetProviderName() const { return "openai"; }

std::string OpenAiClient::ProbeReachability(const AiClientConfig& cfg) {
    const std::string url = JoinUrl(ResolveBaseUrl(cfg), "/v1/models");
    cpr::Header headers{{"Accept", "application/json"}};
    if (!cfg.ApiKey.empty())
        headers["Authorization"] = std::string("Bearer ") + cfg.ApiKey;

    cpr::Response r =
        cpr::Get(cpr::Url{url}, headers, cpr::ConnectTimeout{cfg.ConnectTimeoutMs}, cpr::Timeout{cfg.TotalTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Ai, NetworkUsageTracker::kEstimatedGetUploadBytes, r);
    if (r.error.code != cpr::ErrorCode::OK)
        return std::string("transport: ") + r.error.message;
    if (r.status_code < 200 || r.status_code >= 300)
        return std::string("HTTP ") + std::to_string(r.status_code);
    return std::string();
}

void OpenAiClient::SendStreaming(const AiClientConfig& cfg, const AiChatRequest& req, const DeltaCallback& onDelta,
                                 const ErrorCallback& onError, const CancelToken& cancel) {
    const std::string url = JoinUrl(ResolveBaseUrl(cfg), "/v1/chat/completions");
    const std::string body = BuildChatBody(req).dump();

    cpr::Header headers{
        {"Accept", "text/event-stream"},
        {"Content-Type", "application/json"},
        {"Cache-Control", "no-cache"},
    };
    if (!cfg.ApiKey.empty())
        headers["Authorization"] = std::string("Bearer ") + cfg.ApiKey;

    AiSseParser parser;
    bool sawFinal = false;
    bool cancelObserved = false;

    auto translate = [&](const AiSseParser::Event& ev) {
        if (cancelObserved || sawFinal)
            return;
        DispatchOpenAiDataLine(ev.Data, onDelta, sawFinal);
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
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = std::string("transport: ") + r.error.message;
        onError(err);
        return;
    }

    if (r.status_code < 200 || r.status_code >= 300) {
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = std::string("HTTP ") + std::to_string(r.status_code);
        if (!r.text.empty()) {
            err.Message.append(": ");
            err.Message.append(r.text.substr(0, 600));
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
