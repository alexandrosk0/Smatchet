#ifndef SMATCHET_ANTHROPIC_CLIENT_H
#define SMATCHET_ANTHROPIC_CLIENT_H

#include "IAiClient.h"

#include <string>

/// Anthropic Native Messages API implementation (/v1/messages). Reuses
/// `AiSseParser` for the SSE byte-stream, then translates Anthropic-specific
/// event names — `content_block_delta` (token chunks) and `message_stop`
/// (terminal) — into the provider-neutral `AiStreamDelta` surface. Auth uses
/// `x-api-key` + `anthropic-version` headers rather than `Authorization: Bearer`.
class AnthropicClient : public IAiClient {
  public:
    AnthropicClient() = default;
    ~AnthropicClient() override = default;

    std::string GetProviderName() const override;
    std::string ProbeReachability(const AiClientConfig& cfg) override;
    void SendStreaming(const AiClientConfig& cfg, const AiChatRequest& req, const DeltaCallback& onDelta,
                       const ErrorCallback& onError, const CancelToken& cancel) override;
};

#endif
