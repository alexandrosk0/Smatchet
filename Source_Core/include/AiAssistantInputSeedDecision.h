#pragma once

// Pure decision helper for the AI Assistant chat-input seed-from-model mirror.
//
// The AI Assistant panel keeps two copies of the input text: a process-static
// char buffer (`s_inputCharBuf`) that ImGui::InputTextMultiline writes into,
// and a `std::string` (`UiDrawSession::assistantInputBuf`) that the Send path,
// Lua glue, and other consumers read. Each frame the panel runs:
//
//   1. (optional) seed buf <- model (this decision)
//   2. ImGui::InputTextMultiline draws + edits buf
//   3. mirror buf -> model
//
// Step 1 must NOT clobber a between-frame splice that the dictation router
// pushed directly into `s_inputCharBuf` while the model side was still stale.
// The original code re-seeded whenever `bufLen != modelSize` or the bytes
// differed; that was wrong because a Whisper splice grows `bufLen` past
// `modelSize` and the seed would then overwrite the spliced bytes with the
// stale model.
//
// The fix: only seed model -> buf when `modelSize > bufLen`. Otherwise buf is
// the newer side and the buf -> model mirror at step 3 will catch up.
//
// Free-function so the unit test (`AiAssistantInputSeedDecision.test.cpp`)
// can exercise every branch without dragging ImGui or the SmatchetAiAssistantUi
// TU's namespace-private statics into the test binary.

#include <cstddef>

namespace smatchet::ai {

/// Returns true when the AI Assistant input frame should copy
/// `model -> buf` before InputTextMultiline draws. `seeded` is the
/// "have-we-ever-seeded" sticky flag (false on the first frame the panel
/// renders this session); `bufLen` is `strlen(buf)` and `modelSize` is the
/// `std::string::size()` of the model-side buffer.
inline bool ShouldSeedAssistantInputFromModel(bool seeded, std::size_t bufLen,
                                              std::size_t modelSize, bool bytesDiffer) {
    if (!seeded) {
        // First frame — buf is empty / freshly hydrated; the seed must run so
        // Lua-supplied text from an earlier session survives a panel reopen.
        return true;
    }
    // After the first frame, only seed when the model genuinely has MORE
    // content than the buf — that is the only direction in which the model
    // is the newer side. A between-frame Whisper splice grows bufLen past
    // modelSize; a Lua poke that REPLACES the model with the same-or-shorter
    // text takes effect via the per-frame buf <- string mirror without a
    // seed re-copy (this is the trade-off — Lua replacing the model with
    // text shorter than the current input buf is a deliberate non-action).
    // `bytesDiffer` is kept in the signature so the caller can still compute
    // a "divergence" log line; the decision itself ignores it because the
    // size-based comparison subsumes it.
    (void)bytesDiffer;
    return modelSize > bufLen;
}

} // namespace smatchet::ai
