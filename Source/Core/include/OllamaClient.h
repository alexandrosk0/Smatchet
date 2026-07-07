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

/// Test/observability seam: the exact JSON body OllamaClient POSTs to `/api/chat`
/// for `req`, serialized. Exposed so unit tests can assert the request shape (e.g.
/// that the system prompt is a leading {role:"system"} message) without a live server.
std::string OllamaBuildRequestBodyJson(const AiChatRequest& req);

#endif
