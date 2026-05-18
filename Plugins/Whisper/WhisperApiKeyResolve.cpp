// WhisperApiKeyResolve — see header for the decision-table contract.

#include "WhisperApiKeyResolve.h"

namespace smatchet {
namespace whisper {
namespace pure {

std::string ResolveWhisperApiKey(const std::string& whisperApiKey,
                                 const std::string& aiProvider,
                                 const std::string& aiApiKey) {
    // Row 1: explicit Whisper key always wins, regardless of provider.
    if (!whisperApiKey.empty()) {
        return whisperApiKey;
    }
    // Rows 2/3: fall back to the AI Assistant's key only when the assistant is
    // configured against OpenAI proper. Same-key UX without coupling the two
    // features — if the user later switches AiProvider to Anthropic the
    // Whisper path stops auto-borrowing and surfaces the configure-key prompt.
    if (aiProvider == "openai") {
        return aiApiKey; // empty string is a valid (and meaningful) result
    }
    // Rows 4/5: anything non-OpenAI on the assistant side cannot supply a
    // Whisper key. Anthropic doesn't expose Whisper; ollama / lmstudio /
    // ollama-openai are local-only and have no transcription endpoint.
    return std::string();
}

} // namespace pure
} // namespace whisper
} // namespace smatchet
