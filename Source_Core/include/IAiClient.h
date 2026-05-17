#ifndef SMATCHET_I_AI_CLIENT_H
#define SMATCHET_I_AI_CLIENT_H

#include "AiTypes.h"

#include <functional>
#include <string>

class IAiClient {
  public:
    virtual ~IAiClient() {}

    virtual std::string GetProviderName() const = 0;

    /// Worker-thread only. Returns empty string on success; populated error message otherwise.
    virtual std::string ProbeReachability(const AiClientConfig& cfg) = 0;

    using DeltaCallback = std::function<void(const AiStreamDelta&)>;
    using ErrorCallback = std::function<void(const AiStreamError&)>;
    using CancelToken = AiCancelToken;

    /// Worker-thread only. Blocks until the stream completes, errors, or is cancelled.
    /// onDelta is invoked once per coalesced chunk; onError is invoked at most once at termination.
    virtual void SendStreaming(const AiClientConfig& cfg, const AiChatRequest& req, const DeltaCallback& onDelta,
                               const ErrorCallback& onError, const CancelToken& cancel) = 0;
};

#endif
