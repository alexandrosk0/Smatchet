#pragma once

// Pure decision helper for the AI Assistant Send button's disabled state.
// The Send button in the Smatchet Assistant side panel is disabled when ANY
// of three independent conditions hold:
//   1. A turn is already in flight (`assistantInFlight == true`). Two
//      concurrent submits would corrupt `assistantStreamBuf` + race the
//      worker queue's turn-gen handshake.
//   2. The input buffer is empty — there is nothing to submit. Whitespace
//      is intentionally allowed; the caller decides whether to strip.
//   3. No live `AiAssistantController` exists. This is the
//      `app.HasAiAssistantController() == false` path — usually mid-shutdown
//      or in a stripped non-AI build.
// Historical bug: `assistantInFlight` was set to `true` unconditionally by
// the UI's `dispatchSend` lambda BEFORE the controller's `Submit` was
// invoked. When `Submit` short-circuited (no live client / queue
// shuttingDown), the flag stayed `true` for the lifetime of the app — the
// button stuck disabled, no error strip visible, no recovery path. Fixed
// by making `Submit` return a bool ack and only flipping the flag on
// success; this helper sits in the predicate site so both production code
// and tests share one source of truth for the disabled-state semantics.
// Free-function so the unit test (`AiAssistantSendButton.test.cpp`) can
// exercise every branch without dragging ImGui or the SmatchetAiAssistantUi
// TU's namespace-private statics into the test binary.

namespace smatchet {
namespace ai {

/// Returns true when the AI Assistant Send button should render as disabled.
/// All three inputs are independently fatal — if any is true the button is
/// disabled. No precedence assumptions; callers may surface a per-condition
/// tooltip or status strip separately.
/// `historyLoading`: the saved conversation is still being read from the local cache. A turn sent meanwhile would be
/// replaced by the loaded rows and never stored, so Send waits for it.
inline bool DecideSendDisabled(bool inFlight, bool inputEmpty, bool hasController, bool historyLoading = false) {
    return inFlight || inputEmpty || !hasController || historyLoading;
}

/// What Esc does in the Assistant panel's input row.
enum class AiEscAction { None, StopReply, LeaveInput };

/// `panelHasKeyboard`: the panel or one of its child windows has keyboard focus, never a popup
/// above it. While a reply is coming in, Esc stops it and leaves the caret alone (the next
/// message may be half typed). Idle with the caret in the message box, Esc leaves the box.
/// Otherwise nothing: the panel is a docked window, not a dialog, so Esc never closes it.
inline AiEscAction DecideAssistantEsc(bool escPressed, bool panelHasKeyboard, bool inFlight, bool hasController,
                                      bool caretInInput) {
    if (!escPressed || !panelHasKeyboard) {
        return AiEscAction::None;
    }
    if (inFlight && hasController) {
        return AiEscAction::StopReply;
    }
    return caretInInput ? AiEscAction::LeaveInput : AiEscAction::None;
}

} // namespace ai
} // namespace smatchet
