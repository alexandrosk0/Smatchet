// AiEndpointPolicy.h — maps a TrackerConfig plus an AiProvider to the
// per-provider endpoint-validation policy consumed by
// smatchet::ai::pure::SanitizeAiEndpointUrl. The policy carries the host pin
// plus the insecure-http consent flag.
// Single source of truth so every endpoint-validation site enforces the SAME
// policy: the live request in AiAssistantController, the Preferences
// Test-connection probe, and the Preferences inline validation. Otherwise a
// host that Test-connection accepts could be rejected by the real request, or
// vice versa. This helper is NOT pure — it depends on ConfigManager and AiTypes
// — so it lives outside the cpr-free AiEndpointSanitize TU.

#ifndef SMATCHET_AI_ENDPOINT_POLICY_H
#define SMATCHET_AI_ENDPOINT_POLICY_H

#include "AiEndpointSanitize.h"
#include "AiTypes.h"

struct TrackerConfig;

namespace smatchet {
namespace ai {

// Cloud providers OpenAi and Anthropic are pinned to their canonical host over
// https unless the user granted the per-provider custom-endpoint consent via
// cfg.AiAllowCustomEndpoint*. Local / OpenAI-compat providers — Ollama and
// DeepSeek — stay unpinned and proxy-friendly through the permissive
// EndpointPolicy defaults.
pure::EndpointPolicy EndpointPolicyForProvider(const TrackerConfig& cfg, AiProvider provider);

} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_ENDPOINT_POLICY_H
