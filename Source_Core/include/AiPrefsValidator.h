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

// Field identifiers for structured issue records. Used by the Preferences UI to
// render per-field error / warning glyphs next to the relevant input. Kept as a
// stringly-typed key (rather than an enum) so the Preferences UI table doesn't
// need to enumerate every variant — unknown keys fall through to the banner only.
namespace PrefsFieldKey {
constexpr const char* kAiApiKey = "ai_api_key";
constexpr const char* kAiAnthropicApiKey = "ai_anthropic_api_key";
constexpr const char* kAiBaseUrl = "ai_base_url";
constexpr const char* kAiOllamaBaseUrl = "ai_ollama_base_url";
constexpr const char* kAiModelOpenAi = "ai_model_openai";
constexpr const char* kAiModelAnthropic = "ai_model_anthropic";
constexpr const char* kAiModelOllama = "ai_model_ollama";
} // namespace PrefsFieldKey

enum class PrefsSeverity { Error, Warning };

// One field-level issue. `FieldKey` may be empty for global / cross-field issues.
struct PrefsValidationIssue {
    std::string FieldKey;
    std::string Message;
    PrefsSeverity Severity;
};

struct PrefsValidation {
    std::vector<std::string> Errors;   // Save blocked.
    std::vector<std::string> Warnings; // Save proceeds; banner informs.
    // Structured form of the same errors / warnings, with a field-id mapping so
    // the Preferences UI can render per-field glyphs. `Issues` is the union of
    // Errors + Warnings preserving emission order. Existing callers can keep
    // reading the flat string vectors.
    std::vector<PrefsValidationIssue> Issues;
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
