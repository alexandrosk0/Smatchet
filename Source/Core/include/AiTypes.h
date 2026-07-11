#ifndef SMATCHET_AI_TYPES_H
#define SMATCHET_AI_TYPES_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class AiProvider : int {
    OpenAi = 0,
    Anthropic = 1,
    OllamaOpenAiCompat = 2,
    OllamaNative = 3,
    DeepSeek = 4,
};

// Canonical mapping from the persisted cfg.AiProviderKind int onto the enum,
// clamping any out-of-range value to OpenAi. Single source of truth so a new
// provider cannot be added to the enum while a consumer's local switch silently
// drifts — the DR19 regression, where DeepSeek (kind 4) was missed in two
// disconnected copies. Consumers (AiResolveProvider, ai.validate-prefs) delegate.
inline AiProvider AiProviderFromKind(int kind) {
    switch (kind) {
    case 1:
        return AiProvider::Anthropic;
    case 2:
        return AiProvider::OllamaOpenAiCompat;
    case 3:
        return AiProvider::OllamaNative;
    case 4:
        return AiProvider::DeepSeek;
    case 0:
    default:
        return AiProvider::OpenAi;
    }
}

struct AiMessage {
    std::string Role;
    std::string Content;
    /// Wall-clock unix-epoch milliseconds the message was created locally.
    /// Stamped at dispatch on the UI thread (user) or at assistant-finalisation
    /// on the MainThreadDispatcher (assistant). Wire serializers in
    /// OpenAiClient / AnthropicClient / OllamaClient build the provider JSON
    /// per-field (`role` + `content` only) so these new members never leak
    /// to the upstream LLM API.
    std::int64_t CreatedAtUnixMs = 0; // sentinel 0 = unset/unstamped timestamp.
    /// True when the user has pinned this message via the hover action row.
    /// Survives across runs via the SQLite chat-history persistence layer.
    bool Pinned = false;
};

struct AiStreamDelta {
    std::string TokenChunk;
    bool IsFinal;
    std::string FinishReason; // sentinel empty = no finish reason (only set when IsFinal).
    AiStreamDelta() : IsFinal(false) {}
};

struct AiStreamError {
    int HttpStatus; // sentinel 0 = no HTTP status (local/transport error, not a server reply).
    std::string Message;
    bool WasCancelled;
    AiStreamError() : HttpStatus(0), WasCancelled(false) {}
};

struct AiChatRequest {
    std::string Model;
    std::string SystemPrompt;
    std::vector<AiMessage> History;
    float Temperature; // sentinel < 0 = unset (omit wire param, server default).
    int MaxTokens;     // sentinel 0 = unset (omit wire param, server default).
    /// Reasoning effort for o-series / reasoning-tuned models. Empty or "auto"
    /// = omit the wire parameter (server picks). Recognised values: "low",
    /// "medium", "high". Providers that don't understand the param will ignore
    /// it (OpenAi proper drops unknowns; LM Studio passes it through to local
    /// reasoning models such as Qwen3 / gemma-3).
    std::string ReasoningEffort;
    AiChatRequest() : Temperature(-1.0f), MaxTokens(0) {}
};

struct AiClientConfig {
    std::string ApiKey;
    std::string BaseUrl;
    int ConnectTimeoutMs;
    int TotalTimeoutMs;
    AiClientConfig() : ConnectTimeoutMs(5000), TotalTimeoutMs(120000) {}
};

enum class AiContextBlockKind : int {
    MultiSelectedTickets = 0,
    VisibleGridRows = 1,
    ActiveTicket = 2,
    ActiveView = 3,
    AuditTrail = 4,
};

struct AiContextBlock {
    AiContextBlockKind Kind;
    std::string Name;
    std::string Body;
    AiContextBlock() : Kind(AiContextBlockKind::ActiveTicket) {}
};

using AiCancelToken = std::shared_ptr<std::atomic<bool>>;

#endif
