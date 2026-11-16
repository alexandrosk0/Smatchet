#pragma once

/// Pure decisions shared by the dialogs that hold typed text: the worklog dialog, the offline conflict
/// resolver, the long-text editor and the field viewer. No ImGui here: the callers read the keys, the
/// focus and the popup state, and these say what follows.
namespace smatchet {
namespace dialogkeys {

/// What Esc does in a dialog this frame.
enum class EscapeAction : unsigned char {
    None,       ///< not the dialog's key (not pressed, or a popup above the dialog took it)
    LeaveField, ///< leave the field being typed in; what was typed stays, the dialog stays open
    Cancel      ///< close the dialog, as its Cancel button does
};

/// Esc cancels the dialog, but never around text in one press: with the caret in a field and
/// something typed (`nothingTyped` false) the first Esc only leaves the field, and the next one
/// cancels. An untouched dialog closes on the first Esc even though the caret starts in a field.
/// `dialogOwnedKeyboardLastFrame` keeps the Esc that just closed a popup above the dialog (the
/// suggestions, a date picker, the templates) from also cancelling the dialog under it.
inline EscapeAction ResolveEscape(bool escPressed, bool dialogOwnsKeyboard, bool dialogOwnedKeyboardLastFrame,
                                  bool fieldActive, bool nothingTyped) {
    if (!escPressed || !dialogOwnsKeyboard) {
        return EscapeAction::None;
    }
    if (fieldActive) {
        return nothingTyped ? EscapeAction::Cancel : EscapeAction::LeaveField;
    }
    return dialogOwnedKeyboardLastFrame ? EscapeAction::Cancel : EscapeAction::None;
}

/// What becomes of a request to put the caret in a dialog's field.
enum class CaretRequest : unsigned char {
    Wait, ///< keep the request for a later frame
    Drop, ///< forget the request; the caret stays where it is
    Take  ///< put the caret in the field now
};

/// The request waits while Enter is held: the Enter that opened the dialog would otherwise repeat into
/// the field. It also waits while a window or a payload is being dragged, when a focus request is lost.
/// A request made while something above the dialog has the keyboard is dropped, not kept: the caret is
/// not pulled out of that popup, and not moved later, out of whatever the user went on to.
inline CaretRequest ResolveCaretRequest(bool enterHeld, bool dragInProgress, bool dialogOwnsKeyboard) {
    if (enterHeld || dragInProgress) {
        return CaretRequest::Wait;
    }
    return dialogOwnsKeyboard ? CaretRequest::Take : CaretRequest::Drop;
}

/// A popup that holds typed text was dismissed without its own say: the title-bar X, or another popup
/// took its place. True when it comes back next frame with the discard question up. False when it
/// closes for good: nothing was typed, it is already on its way back, or it was dismissed again within
/// `settleFrames` of the last time it came back (`lastReopenFrame`, negative when it never did), which
/// means something keeps closing it and asking again would loop.
inline bool DismissedPopupComesBack(bool hasUnsavedText, bool comingBackNow, int nowFrame, int lastReopenFrame,
                                    int settleFrames) {
    if (!hasUnsavedText || comingBackNow) {
        return false;
    }
    return lastReopenFrame < 0 || nowFrame - lastReopenFrame > settleFrames;
}

} // namespace dialogkeys
} // namespace smatchet
