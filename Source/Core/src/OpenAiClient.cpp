#include "OpenAiClient.h"

#include "AiErrorRedact.h"
#include "AiSseParser.h"
#include "AiWireIntrospect.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace {

constexpr const char* kDefaultBaseUrl = "https://api.openai.com";
constexpr int kDefaultMaxTokens = 4096;

std::string ResolveBaseUrl(const AiClientConfig& cfg) {
    std::string base = cfg.BaseUrl.empty() ? std::string(kDefaultBaseUrl) : cfg.BaseUrl;
    // Strip a trailing "/v1" or "/v1/" so callers can interchangeably pass
    // "http://localhost:1234" or "http://localhost:1234/v1" — the natural
    // copy-paste from LM Studio / Ollama-OpenAI-compat / OpenAI docs all
    // include the /v1 suffix, which would otherwise produce /v1/v1/... paths.
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, "/v1/") == 0) {
        base.resize(base.size() - 4);
    } else if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) {
        base.resize(base.size() - 3);
    }
    return base;
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
    // Always cap max_tokens so reasoning-style models (e.g. LM Studio's
    // gemma-4-31b) don't burn their entire budget on `reasoning_content`
    // and never reach `content`. Mirrors AnthropicClient::kDefaultMaxTokens.
    body["max_tokens"] = (req.MaxTokens > 0) ? req.MaxTokens : kDefaultMaxTokens;
    // Forward reasoning_effort only when the caller picked an explicit value.
    // OpenAI's o-series + LM Studio's reasoning-model passthrough both accept
    // "low" | "medium" | "high"; other providers silently ignore unknown
    // parameters so it's safe to emit unconditionally per provider — we just
    // skip the "auto" sentinel to keep the wire body minimal.
    if (!req.ReasoningEffort.empty() && req.ReasoningEffort != "auto") {
        body["reasoning_effort"] = req.ReasoningEffort;
    }

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
    // The raw SSE payload is server-supplied and attacker-influenced; parse it through
    // the depth/node-bounded helper so a hostile / oversized stream can't crash the
    // process. ParseBounded never throws and signals failure via a non-empty parseErr.
    std::string parseErr;
    nlohmann::json j = smatchet::json_safe::ParseBounded(data, parseErr);
    if (!parseErr.empty()) {
        // A misconfigured proxy could echo the request Authorization header into a
        // malformed stream, so redact the excerpt (Bearer / api-key shapes) before
        // logging and cap it at 200 chars. parseErr is a stable, input-free message but
        // is run through the same redactor for symmetry. Redaction is the ONLY path
        // provider-error text reaches a log (issue #1286).
        const std::string redactedErr = smatchet::ai::pure::RedactProviderErrorBody(parseErr);
        const std::string redacted = smatchet::ai::pure::RedactProviderErrorBody(data);
        LOG_WARN("OpenAiClient: SSE data not valid JSON (%s): %.*s", redactedErr.c_str(),
                 static_cast<int>(redacted.size() > 200 ? 200 : redacted.size()), redacted.c_str());
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

// Single-source wire introspection (AiWireIntrospect.h) — delegate to the same
// anonymous-namespace builders the live Chat/Stream path uses, so ai.dump-request
// reflects the exact wire body/URL with no drift.
namespace smatchet {
namespace ai {
nlohmann::json OpenAiBuildChatBodyJson(const AiChatRequest& req) { return BuildChatBody(req); }
std::string OpenAiResolveChatUrl(const AiClientConfig& cfg) {
    return JoinUrl(ResolveBaseUrl(cfg), "/v1/chat/completions");
}
} // namespace ai
} // namespace smatchet

std::string OpenAiClient::GetProviderName() const { return "openai"; }

std::string OpenAiClient::ProbeReachability(const AiClientConfig& cfg) {
    const std::string url = JoinUrl(ResolveBaseUrl(cfg), "/v1/models");
    cpr::Header headers{{"Accept", "application/json"}};
    if (!cfg.ApiKey.empty())
        headers["Authorization"] = std::string("Bearer ") + cfg.ApiKey;

    cpr::Response r = cpr::Get(cpr::Url{url}, headers, cpr::Redirect{smatchet::ai::pure::kAiFollowRedirects, false},
                               cpr::ConnectTimeout{cfg.ConnectTimeoutMs}, cpr::Timeout{cfg.TotalTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Ai, NetworkUsageTracker::kEstimatedGetUploadBytes, r);
    if (r.error.code != cpr::ErrorCode::OK)
        return std::string("transport: ") + r.error.message;
    if (r.status_code < 200 || r.status_code >= 300) {
        // Error-path visibility: parallels the LOG_ERROR in SendStreaming(). When the
        // probe fails the bare "HTTP 401" return string told us nothing about *which*
        // key the provider rejected — and Test connection's only feedback is "Failed:
        // HTTP 401". Logging the redacted server body here closes that gap (the body
        // typically carries a key-shape hint like "your api key: ****XYZ is invalid").
        const std::string redactedBody = smatchet::ai::pure::RedactProviderErrorBody(r.text);
        LOG_ERROR("OpenAiClient::ProbeReachability: HTTP %ld at %s - body: %s", static_cast<long>(r.status_code),
                  url.c_str(), redactedBody.c_str());
        return std::string("HTTP ") + std::to_string(r.status_code);
    }
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
                               if (cancel && cancel->load(std::memory_order_acquire)) {
                                   cancelObserved = true;
                                   return false;
                               }
                               parser.Feed(chunk.data(), chunk.size(), translate);
                               if (cancel && cancel->load(std::memory_order_acquire)) {
                                   cancelObserved = true;
                                   return false;
                               }
                               return !sawFinal;
                           },
                           0};

    cpr::Response r = cpr::Post(cpr::Url{url}, headers, cpr::Body{body}, wcb,
                                cpr::Redirect{smatchet::ai::pure::kAiFollowRedirects, false},
                                cpr::ConnectTimeout{cfg.ConnectTimeoutMs}, cpr::Timeout{cfg.TotalTimeoutMs});
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
        LOG_ERROR("OpenAiClient: transport error - cpr code %d, message: %s", static_cast<int>(r.error.code),
                  r.error.message.c_str());
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = std::string("transport: ") + r.error.message;
        onError(err);
        return;
    }

    if (r.status_code < 200 || r.status_code >= 300) {
        // Provider HTTP error visibility - see parallel comment in
        // AnthropicClient.cpp. LOG_ERROR carries the redacted body so users
        // can diagnose 400/401/429/5xx without leaking secrets.
        const std::string redactedBody = smatchet::ai::pure::RedactProviderErrorBody(r.text);
        LOG_ERROR("OpenAiClient: HTTP %ld - body: %s", static_cast<long>(r.status_code), redactedBody.c_str());
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = std::string("HTTP ") + std::to_string(r.status_code);
        if (!redactedBody.empty()) {
            err.Message.append(": ");
            err.Message.append(redactedBody);
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
