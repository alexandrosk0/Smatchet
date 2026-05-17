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
