// AiEndpointPolicy.h — maps a TrackerConfig + AiProvider to the per-provider
// endpoint-validation policy (host pin + insecure-http consent) consumed by
// `smatchet::ai::pure::SanitizeAiEndpointUrl`.
//
// Single source of truth so every endpoint-validation site (the live request in
// AiAssistantController, the Preferences Test-connection probe, and the
// Preferences inline validation) enforces the SAME policy — otherwise a host
// that Test-connection accepts could be rejected by the real request, or vice
// versa. NOT pure: depends on ConfigManager (TrackerConfig) + AiTypes
// (AiProvider), so it lives outside AiEndpointSanitize.cpp's cpr-free TU.

#ifndef SMATCHET_AI_ENDPOINT_POLICY_H
#define SMATCHET_AI_ENDPOINT_POLICY_H

#include "AiEndpointSanitize.h"
#include "AiTypes.h"

struct TrackerConfig;

namespace smatchet {
namespace ai {

// Cloud providers (OpenAi / Anthropic) are pinned to their canonical host + https
// unless the user granted the per-provider custom-endpoint consent
// (`cfg.AiAllowCustomEndpoint*`). Local / OpenAI-compat providers (Ollama,
// DeepSeek) stay unpinned + proxy-friendly via the permissive EndpointPolicy
// defaults.
pure::EndpointPolicy EndpointPolicyForProvider(const TrackerConfig& cfg, AiProvider provider);

} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_ENDPOINT_POLICY_H
