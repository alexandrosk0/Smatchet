#ifndef SMATCHET_AI_CLIENT_FACTORY_H
#define SMATCHET_AI_CLIENT_FACTORY_H

#include "AiTypes.h"
#include "IAiClient.h"

#include <memory>
#include <string>
#include <vector>

namespace AiClientFactory {

/// Returns nullptr for providers not yet implemented in the current build.
std::unique_ptr<IAiClient> MakeAiClient(AiProvider provider);

/// Test seam — when set, `MakeAiClient` calls this function pointer instead of
/// the default provider-kind switch. Pass `nullptr` to clear. Used by bucket-E
/// TUs to inject a stub `IAiClient` for deterministic success/failure paths
/// without spawning real HTTP traffic. Production code MUST NOT call this.
using TestOverrideFn = std::unique_ptr<IAiClient> (*)(AiProvider);
void SetTestOverride(TestOverrideFn fn);

std::string ProviderToString(AiProvider provider);
bool ProviderFromString(const std::string& s, AiProvider& out);

struct ProviderEntry {
    AiProvider Kind;
    std::string Key;     // stable serialised form (lowercase)
    std::string Display; // user-facing label
};

/// Enumerated for the Preferences Combo, stable order regardless of build flags.
std::vector<ProviderEntry> EnumeratedProviders();

} // namespace AiClientFactory

#endif
