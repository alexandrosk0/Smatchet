#include "OllamaClient.h"

#include "AiErrorRedact.h"
#include "AiNdjsonParser.h"
#include "AiWireIntrospect.h"
#include "OllamaStreamError.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace {

constexpr const char* kDefaultBaseUrl = "http://localhost:11434";

std::string ResolveBaseUrl(const AiClientConfig& cfg) {
    if (!cfg.BaseUrl.empty())
        return cfg.BaseUrl;
    return kDefaultBaseUrl;
}

// SMATCHET_DEVIATION(rule=duplication; reason=the AI provider clients (Ollama/OpenAi/Anthropic) share the file-top
// base-URL resolve + path-join helper shape by necessity — each adapts to a different wire schema and folding the
// helpers into one shared unit would couple otherwise-independent provider adapters (DRY Pillar 5 per ADR-0015);
// owner=ai-clients; revisit=2026-12-31)
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

    // Ollama supports an `options` block for sampling parameters.
    if (req.Temperature >= 0.0f) {
        nlohmann::json options;
        options["temperature"] = req.Temperature;
        if (req.MaxTokens > 0)
            options["num_predict"] = req.MaxTokens;
        body["options"] = std::move(options);
    } else if (req.MaxTokens > 0) {
        nlohmann::json options;
        options["num_predict"] = req.MaxTokens;
        body["options"] = std::move(options);
    }

    // SMATCHET_DEVIATION(rule=duplication; reason=the History->messages array-build loop (and the leading system
    // message) is identical across the provider clients by necessity; the bodies otherwise differ (Ollama
    // options.num_predict vs OpenAi max_tokens), and sharing just the loop would couple independent provider adapters
    // (DRY Pillar 5 per ADR-0015); owner=ai-clients; revisit=2026-12-31)
    nlohmann::json messages = nlohmann::json::array();
    // Ollama's /api/chat takes the system prompt as a leading {role:"system"} message.
    // A top-level `system` field is an /api/generate parameter that /api/chat ignores,
    // so emit it as a message to guarantee agents.md + context reach the model.
    if (!req.SystemPrompt.empty()) {
        nlohmann::json sys;
        sys["role"] = "system";
        sys["content"] = req.SystemPrompt;
        messages.push_back(std::move(sys));
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

void DispatchOllamaLine(const nlohmann::json& j, const IAiClient::DeltaCallback& onDelta, bool& sawFinal) {
    if (!j.is_object())
        return;

    AiStreamDelta d;
    if (j.contains("message") && j["message"].is_object()) {
        const auto& msg = j["message"];
        if (msg.contains("content") && msg["content"].is_string())
            d.TokenChunk = msg["content"].get<std::string>();
    }

    bool done = false;
    if (j.contains("done") && j["done"].is_boolean())
        done = j["done"].get<bool>();
    d.IsFinal = done;
    if (done) {
        if (j.contains("done_reason") && j["done_reason"].is_string())
            d.FinishReason = j["done_reason"].get<std::string>();
        else
            d.FinishReason = "stop";
        sawFinal = true;
    }

    if (!d.TokenChunk.empty() || d.IsFinal)
        onDelta(d);
}

} // namespace

std::string OllamaBuildRequestBodyJson(const AiChatRequest& req) { return BuildChatBody(req).dump(); }

// Single-source wire introspection (AiWireIntrospect.h) — see OpenAiClient.cpp.
namespace smatchet {
namespace ai {
nlohmann::json OllamaNativeBuildChatBodyJson(const AiChatRequest& req) { return BuildChatBody(req); }
std::string OllamaNativeResolveChatUrl(const AiClientConfig& cfg) { return JoinUrl(ResolveBaseUrl(cfg), "/api/chat"); }
} // namespace ai
} // namespace smatchet

std::string OllamaClient::GetProviderName() const { return "ollama"; }

std::string OllamaClient::ProbeReachability(const AiClientConfig& cfg) {
    // Ollama exposes a public unauthenticated `/api/tags` endpoint that lists
    // installed models. A 200 with a JSON body confirms reachability.
    const std::string url = JoinUrl(ResolveBaseUrl(cfg), "/api/tags");
    cpr::Header headers{{"Accept", "application/json"}};
    cpr::Response r = cpr::Get(cpr::Url{url}, headers, cpr::Redirect{smatchet::ai::pure::kAiFollowRedirects, false},
                               cpr::ConnectTimeout{cfg.ConnectTimeoutMs}, cpr::Timeout{cfg.TotalTimeoutMs});
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Ai, NetworkUsageTracker::kEstimatedGetUploadBytes, r);
    if (r.error.code != cpr::ErrorCode::OK)
        return std::string("transport: ") + r.error.message;
    if (r.status_code < 200 || r.status_code >= 300)
        return std::string("HTTP ") + std::to_string(r.status_code);
    return std::string();
}

void OllamaClient::SendStreaming(const AiClientConfig& cfg, const AiChatRequest& req, const DeltaCallback& onDelta,
                                 const ErrorCallback& onError, const CancelToken& cancel) {
    const std::string url = JoinUrl(ResolveBaseUrl(cfg), "/api/chat");
    const std::string body = BuildChatBody(req).dump();

    cpr::Header headers{
        {"Accept", "application/x-ndjson"},
        {"Content-Type", "application/json"},
    };
    // Ollama-native is local + keyless, but a user-set OpenAI-compat BaseUrl can
    // point elsewhere — disable redirect-following uniformly with the other AI
    // clients (defense-in-depth, security synthesis #11).

    AiNdjsonParser parser;
    bool sawFinal = false;
    bool cancelObserved = false;
    bool sawStreamError = false;
    std::string streamErrorMessage;

    auto onLine = [&](const nlohmann::json& j) {
        if (cancelObserved || sawFinal)
            return;
        // A mid-stream NDJSON `{"error": "..."}` line on an otherwise-200 stream is a
        // real failure. Record it and mark the stream terminated (sawFinal) exactly as
        // a normal terminal line does, leaving the shared post-response tail untouched.
        // The trailing sawStreamError branch then reports it via onError instead of
        // synthesizing a success `eof` that would commit truncated text (DR20).
        if (j.is_object() && j.contains("error") && j["error"].is_string()) {
            streamErrorMessage = smatchet::ai::pure::RedactProviderErrorBody(j["error"].get<std::string>());
            sawStreamError = true;
            sawFinal = true;
            return;
        }
        DispatchOllamaLine(j, onDelta, sawFinal);
    };
    auto onParseError = [](const std::string& rawLine) {
        // The raw NDJSON line is server-supplied; a misconfigured proxy could echo the
        // request Authorization header into a malformed stream. Redact (Bearer / api-key
        // shapes) before logging, then cap the excerpt at 200 chars.
        const std::string redacted = smatchet::ai::pure::RedactProviderErrorBody(rawLine);
        LOG_WARN("OllamaClient: NDJSON line not valid JSON: %.*s",
                 static_cast<int>(redacted.size() > 200 ? 200 : redacted.size()), redacted.c_str());
    };

    cpr::WriteCallback wcb{[&](const std::string& chunk, intptr_t) -> bool {
                               if (cancel && cancel->load(std::memory_order_acquire)) {
                                   cancelObserved = true;
                                   return false;
                               }
                               parser.Feed(chunk.data(), chunk.size(), onLine, onParseError);
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
        parser.Flush(onLine, onParseError);

    if (cancelObserved) {
        AiStreamError err;
        err.WasCancelled = true;
        err.Message = "Cancelled by user";
        onError(err);
        return;
    }

    if (r.error.code != cpr::ErrorCode::OK && r.error.code != cpr::ErrorCode::REQUEST_CANCELLED) {
        LOG_ERROR("OllamaClient: transport error - cpr code %d, message: %s", static_cast<int>(r.error.code),
                  r.error.message.c_str());
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = smatchet::ai::pure::FormatOllamaTransportError(r.error.message);
        onError(err);
        return;
    }

    if (r.status_code < 200 || r.status_code >= 300) {
        // Provider HTTP error visibility - see parallel comment in
        // AnthropicClient.cpp. LOG_ERROR carries the redacted body so users
        // can diagnose 400/401/429/5xx without leaking secrets.
        const std::string redactedBody = smatchet::ai::pure::RedactProviderErrorBody(r.text);
        LOG_ERROR("OllamaClient: HTTP %ld - body: %s", static_cast<long>(r.status_code), redactedBody.c_str());
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = smatchet::ai::pure::FormatOllamaHttpError(r.status_code, redactedBody);
        onError(err);
        return;
    }

    if (!sawFinal) {
        AiStreamDelta d;
        d.IsFinal = true;
        d.FinishReason = "eof";
        onDelta(d);
    }

    if (sawStreamError) {
        // Mid-stream provider `error` line on a 200 stream: transport/HTTP above saw
        // a clean 200 and the eof above was suppressed by the sawFinal flag set at
        // detection, so surface the failure here rather than as success (DR20).
        AiStreamError err;
        err.HttpStatus = r.status_code;
        err.Message = streamErrorMessage.empty() ? std::string("provider stream error") : streamErrorMessage;
        onError(err);
    }
}
