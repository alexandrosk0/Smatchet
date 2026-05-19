#ifndef SMATCHET_AI_TYPES_H
#define SMATCHET_AI_TYPES_H

#include <atomic>
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

struct AiMessage {
    std::string Role;
    std::string Content;
};

struct AiStreamDelta {
    std::string TokenChunk;
    bool IsFinal;
    std::string FinishReason;
    AiStreamDelta() : IsFinal(false) {}
};

struct AiStreamError {
    int HttpStatus;
    std::string Message;
    bool WasCancelled;
    AiStreamError() : HttpStatus(0), WasCancelled(false) {}
};

struct AiChatRequest {
    std::string Model;
    std::string SystemPrompt;
    std::vector<AiMessage> History;
    float Temperature;
    int MaxTokens;
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
