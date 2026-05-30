// AiModelCatalog — fixed list of public model identifiers per provider.
// Seeds the Preferences model dropdown and the AiPrefsValidator's
// unknown-model warning. Pure C++14, no network — keeps the doctest rig
// link-clean.
// Catalog is hand-maintained from the providers' public model docs. When a
// provider rotates models, edit AiModelCatalog.cpp; the validator's
// "unknown model" warning surface guides users toward valid IDs without
// blocking save.

#ifndef SMATCHET_AI_MODEL_CATALOG_H
#define SMATCHET_AI_MODEL_CATALOG_H

#include "AiTypes.h"

#include <string>
#include <vector>

namespace smatchet {
namespace ai {

struct ModelOption {
    std::string Id;          // Sent verbatim as the API `model` field.
    std::string DisplayName; // Shown in the Preferences Combo.
};

// Returns the published catalog for `provider`. Empty vector means the
// provider has no fixed catalog — typically Ollama paths whose models are
// user-installed and discovered locally.
std::vector<ModelOption> KnownModels(AiProvider provider);

// True if `modelId` exactly matches one of `KnownModels(provider)`.
// `false` when the catalog is empty (a no-catalog provider has no known IDs,
// so every model is "unknown" in this sense — but the validator skips the
// warning for empty-catalog providers).
bool IsKnownModel(AiProvider provider, const std::string& modelId);

} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_MODEL_CATALOG_H
