// AiPrefsValidator — pure-logic validation of the Assistant Preferences tab.
//
// Two severities:
//   - Errors block save and surface as a red banner.
//   - Warnings save through and surface as a yellow banner.
//
// Inspects only the `Ai*` fields on `TrackerConfig` plus the active provider's
// model catalog (via AiModelCatalog). No network. No UI dependency. Lives in
// the doctest rig.

#ifndef SMATCHET_AI_PREFS_VALIDATOR_H
#define SMATCHET_AI_PREFS_VALIDATOR_H

#include "ConfigManager.h"

#include <string>
#include <vector>

namespace smatchet {
namespace ai {

struct PrefsValidation {
    std::vector<std::string> Errors;   // Save blocked.
    std::vector<std::string> Warnings; // Save proceeds; banner informs.
    bool IsOk() const { return Errors.empty(); }
};

// Validate the Assistant subset of `cfg`. Active provider is read from
// `cfg.AiProviderKind` (clamped to 0..3); per-provider checks fire only for
// the active provider so a stale OpenAI key while running Anthropic doesn't
// nag.
PrefsValidation ValidateAiPrefs(const TrackerConfig& cfg);

} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_PREFS_VALIDATOR_H
