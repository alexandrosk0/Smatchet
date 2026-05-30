#ifndef SMATCHET_OPEN_AI_CLIENT_H
#define SMATCHET_OPEN_AI_CLIENT_H

#include "IAiClient.h"

#include <string>

/// OpenAI Chat Completions implementation. Also drives OpenAI-compatible
/// endpoints (Ollama-OpenAI-compat, Azure, Groq, Together) — the wire format
/// is identical; only the BaseUrl + ApiKey differ.
class OpenAiClient : public IAiClient {
  public:
    OpenAiClient() = default;
    ~OpenAiClient() override = default;

    std::string GetProviderName() const override;
    std::string ProbeReachability(const AiClientConfig& cfg) override;
    void SendStreaming(const AiClientConfig& cfg, const AiChatRequest& req, const DeltaCallback& onDelta,
                       const ErrorCallback& onError, const CancelToken& cancel) override;
};

#endif
