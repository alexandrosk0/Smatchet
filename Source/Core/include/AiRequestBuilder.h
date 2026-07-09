// AiRequestBuilder.h — single production seam for provider -> AiClientConfig
// resolution: per-provider API-key selection through the header sanitizer plus the
// endpoint sanitize-with-consent gate. Consumed by the live AiAssistantController
// turn path AND the debug ai.* CLI commands (ai.dump-request / ai.probe /
// ai.send-once), so the debug dump shows the production wire config instead of
// drifting behind a clone (AGENTIC_INFRA_AUDIT.md finding B4 / proposal P4).

#ifndef SMATCHET_AI_REQUEST_BUILDER_H
#define SMATCHET_AI_REQUEST_BUILDER_H

#include "AiTypes.h"

#include <string>

struct TrackerConfig;

namespace smatchet {
namespace ai {

/// Strip CR/LF/NUL from a header-bound value (API keys). libcurl usually rejects
/// them already; defense-in-depth so a user-typed `cfg.Ai*ApiKey` never carries
/// header-smuggling control characters into an outbound request.
std::string SanitizeHeaderValue(const std::string& value);

/// Resolve the production `AiClientConfig` for `provider` from `cfg`: per-provider
/// key selection through `SanitizeHeaderValue`, the base-URL fallback chains, and
/// the `EndpointPolicyForProvider` sanitize-with-consent gate (a rejected URL
/// yields an empty BaseUrl and a structured log line, so clients fall back to
/// their built-in default). Applies the 10-minute streaming TotalTimeoutMs;
/// probe-style callers override the timeouts afterwards.
AiClientConfig BuildClientConfig(const TrackerConfig& cfg, AiProvider provider);

} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_REQUEST_BUILDER_H
