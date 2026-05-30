// AiModelSignature — pure helper for the F2 "model change auto-clears chat
// history" rule. The controller caches the previous turn's signature ("provider|model")
// and asks this helper whether the freshly resolved signature implies a clear.
// Rules encoded here (all unit-tested in tests/Core/AiModelSignature.test.cpp):
//   * Empty previous signature -> never clear (first turn, fresh session).
//   * Identical new signature -> never clear.
//   * Different provider or different model -> clear.
//   * Reasoning effort never enters the signature -> effort-only changes never clear.
// Plain string concatenation suffices for the signature; the chosen separator
// ("|") is not a legal character in any AiProvider key (lowercase ASCII) or any
// published model identifier (alphanumerics + `-` + `.` + `_`).

#ifndef SMATCHET_AI_MODEL_SIGNATURE_H
#define SMATCHET_AI_MODEL_SIGNATURE_H

#include <string>

namespace smatchet {
namespace ai {

struct ModelSignatureChangeResult {
    bool ShouldClear;
    std::string NewSignature;
};

ModelSignatureChangeResult DetectModelChange(const std::string& previousSignature, const std::string& providerName,
                                             const std::string& model);

} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_MODEL_SIGNATURE_H
