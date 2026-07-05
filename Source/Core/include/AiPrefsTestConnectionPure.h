#pragma once

#if defined(SMATCHET_WITH_AI)

#include "AiEndpointSanitize.h"
#include "AiTypes.h"
#include "ConfigManager.h"

#include <string>

/**
 * Pure pre-network half of the Test-connection probe (AiPrefsTestConnection.cpp is the shell
 * that runs the actual reachability + 1-token chat handshake on a background task). Everything
 * that decides WHAT the probe sends lives here: which credential slot / base URL / model each
 * provider reads, the local-provider default-URL fallback, and the client-config assembly with
 * its header-smuggling key strip + endpoint-sanitiser gate. Credential routing bugs of the
 * CPP_CODE_AUDIT.md #2 class (wrong slot picked per provider) are pinned by tests here.
 *
 * Owner: AiPrefsTestConnection (preferences Test-connection probe) — pure helpers live here.
 * Test surface: tests/Core/AiPrefsTestConnectionPure.test.cpp.
 */
namespace AiPrefsTestConnectionPure {

/// Provider-aware ApiKey / BaseUrl / ModelId pick — mirrors `BuildClientConfig`
/// + `ResolveModelId` in `AiAssistantController.cpp`.
struct ProbeTarget {
    std::string ApiKey;
    std::string BaseUrl;
    std::string ModelId;
};

ProbeTarget ResolveProbeTarget(const TrackerConfig& cfg, AiProvider provider);

/// Canonical default base URL for a local/hosted provider when the configured slot
/// is empty. Empty string for providers that have no built-in default (OpenAi,
/// Anthropic) so the client uses its own default.
std::string DefaultBaseUrlFor(AiProvider provider);

/// Build the probe's AiClientConfig from the resolved key + base URL. Strips
/// header-smuggling control characters from the key, then runs the base URL through
/// the endpoint sanitiser, falling back to the empty provider default on rejection.
AiClientConfig BuildProbeClientConfig(const std::string& apiKey, const std::string& baseUrl,
                                      const smatchet::ai::pure::EndpointPolicy& policy);

/// The complete pre-network probe decision: resolved client config + model, plus the
/// default base URL that was substituted for an empty configured slot (recorded so the
/// success callback can persist it back into cfg; empty when no defaulting happened).
struct ProbePlan {
    AiClientConfig ClientCfg;
    std::string ModelId;
    std::string DefaultedBaseUrl;
};

ProbePlan PlanProbe(const TrackerConfig& cfg, AiProvider provider, const smatchet::ai::pure::EndpointPolicy& policy);

} // namespace AiPrefsTestConnectionPure

#endif // SMATCHET_WITH_AI
