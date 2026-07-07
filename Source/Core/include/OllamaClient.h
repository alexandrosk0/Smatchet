#ifndef SMATCHET_OLLAMA_CLIENT_H
#define SMATCHET_OLLAMA_CLIENT_H

#include "IAiClient.h"

#include <string>

/// Ollama native `/api/chat` implementation. Uses newline-delimited JSON
/// (NDJSON) for the streaming wire format — one complete JSON object per line,
/// terminated by `"done":true`. Parsed via `AiNdjsonParser`. Local-only;
/// `cfg.ApiKey` is ignored. Default `cfg.BaseUrl` is `http://localhost:11434`.
class OllamaClient : public IAiClient {
  public:
    OllamaClient() = default;
    ~OllamaClient() override = default;

    std::string GetProviderName() const override;
    std::string ProbeReachability(const AiClientConfig& cfg) override;
    void SendStreaming(const AiClientConfig& cfg, const AiChatRequest& req, const DeltaCallback& onDelta,
                       const ErrorCallback& onError, const CancelToken& cancel) override;
};

#endif
