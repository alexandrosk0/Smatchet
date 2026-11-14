// AiAssistantSendButton — unit gate for the AI Assistant Send-button
// disabled-state predicate. Pinned regression: prior to this slice, the
// production code set `assistantInFlight = true` unconditionally before
// calling `AiAssistantController::Submit`; when Submit dropped the call
// (no live client), the flag stayed true forever and the button stuck
// disabled. The fix has two halves:
//
//   - Submit now returns a bool ack; the UI flips inFlight only on true.
//   - This helper centralises the disabled-state predicate so both
//     production code and the test rig share one rule.
//
// See Source/Core/include/AiAssistantSendButton.h for the helper +
// rationale.

#include <doctest/doctest.h>

#include "AiAssistantSendButton.h"

using smatchet::ai::AiEscAction;
using smatchet::ai::DecideAssistantEsc;
using smatchet::ai::DecideSendDisabled;

TEST_CASE("DecideAssistantEsc: no Esc, or the panel without the keyboard, does nothing") {
    for (int bits = 0; bits < 8; ++bits) {
        const bool inFlight = (bits & 1) != 0;
        const bool hasController = (bits & 2) != 0;
        const bool caretInInput = (bits & 4) != 0;
        CHECK(DecideAssistantEsc(/*escPressed=*/false, /*panelHasKeyboard=*/true, inFlight, hasController,
                                 caretInInput) == AiEscAction::None);
        // A popup above the panel, or another window, has the keyboard: its Esc is not ours.
        CHECK(DecideAssistantEsc(/*escPressed=*/true, /*panelHasKeyboard=*/false, inFlight, hasController,
                                 caretInInput) == AiEscAction::None);
    }
}

TEST_CASE("DecideAssistantEsc: a reply coming in is stopped, caret in the box or not") {
    CHECK(DecideAssistantEsc(true, true, /*inFlight=*/true, /*hasController=*/true, /*caretInInput=*/true) ==
          AiEscAction::StopReply);
    CHECK(DecideAssistantEsc(true, true, /*inFlight=*/true, /*hasController=*/true, /*caretInInput=*/false) ==
          AiEscAction::StopReply);
}

TEST_CASE("DecideAssistantEsc: a stuck in-flight flag with no controller never asks for a stop") {
    CHECK(DecideAssistantEsc(true, true, /*inFlight=*/true, /*hasController=*/false, /*caretInInput=*/true) ==
          AiEscAction::LeaveInput);
    CHECK(DecideAssistantEsc(true, true, /*inFlight=*/true, /*hasController=*/false, /*caretInInput=*/false) ==
          AiEscAction::None);
}

TEST_CASE("DecideAssistantEsc: idle, Esc leaves the message box and never closes the panel") {
    CHECK(DecideAssistantEsc(true, true, /*inFlight=*/false, /*hasController=*/true, /*caretInInput=*/true) ==
          AiEscAction::LeaveInput);
    CHECK(DecideAssistantEsc(true, true, /*inFlight=*/false, /*hasController=*/true, /*caretInInput=*/false) ==
          AiEscAction::None);
}

TEST_CASE("DecideSendDisabled: all-false enables the button") {
    // Live controller, non-empty input, no in-flight turn — Send is the
    // canonical Ready state.
    CHECK_FALSE(DecideSendDisabled(/*inFlight=*/false, /*inputEmpty=*/false,
                                   /*hasController=*/true));
}

TEST_CASE("DecideSendDisabled: in-flight disables (single-turn guard)") {
    // Two simultaneous turns would corrupt the stream buffer and race the
    // worker queue's turn-gen handshake. The button must NOT accept a
    // second click while the first is still streaming.
    CHECK(DecideSendDisabled(/*inFlight=*/true, /*inputEmpty=*/false,
                             /*hasController=*/true));
}

TEST_CASE("DecideSendDisabled: empty input disables (nothing-to-send guard)") {
    // The user has not typed anything; sending an empty prompt is
    // wasted work + ambiguous server-side behaviour. Whitespace is
    // intentionally NOT stripped here — the caller's policy.
    CHECK(DecideSendDisabled(/*inFlight=*/false, /*inputEmpty=*/true,
                             /*hasController=*/true));
}

TEST_CASE("DecideSendDisabled: missing controller disables (no-AI-build guard)") {
    // `app.HasAiAssistantController() == false` — usually mid-shutdown,
    // or a stripped non-AI build accidentally rendering the panel.
    CHECK(DecideSendDisabled(/*inFlight=*/false, /*inputEmpty=*/false,
                             /*hasController=*/false));
}

TEST_CASE("DecideSendDisabled: any single condition disables (independent guards)") {
    // The three conditions are independently fatal. Verify every
    // single-bit-set state disables.
    CHECK(DecideSendDisabled(true, false, true));
    CHECK(DecideSendDisabled(false, true, true));
    CHECK(DecideSendDisabled(false, false, false));
}

TEST_CASE("DecideSendDisabled: combinations of conditions still disable") {
    // Defense-in-depth — any combination of true / false-hasController
    // bits keeps the button disabled. The only path to enabled is the
    // all-Ready state asserted by the first test case above.
    CHECK(DecideSendDisabled(true, true, true));
    CHECK(DecideSendDisabled(true, false, false));
    CHECK(DecideSendDisabled(false, true, false));
    CHECK(DecideSendDisabled(true, true, false));
}
