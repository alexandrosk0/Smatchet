#pragma once

// One keyboard contract for modal dialogs (input-ux-w2-dialog-contract; audit § 3 rows "Form dialog"
// and "Confirm dialog", findings #85 #86 #88 #91 #94).
//
// ImGui 1.92 leaves a modal's keys to the caller: its own Esc handling closes only non-modal popups, a
// modal opens with nothing focused, and Enter does nothing unless the keyboard cursor is visible on a
// button. Each dialog used to decide for itself. This scope decides once:
//   - On open, the dialog's first field (a form) or its default button (a confirm) takes the keyboard.
//   - Esc cancels (smatchet::dialogkeys::ResolveEscape: a field with typed text is left first).
//   - Enter runs the default button of a confirm; a destructive confirm defaults to its safe button.
//   - Ctrl+Enter is a form's submit.
//   - App-wide shortcuts are muted while any modal is up (AnyModalDialogOpen, read by
//     SmatchetUI::dispatchKeybindings).
// The decisions are pure and live in Ui/DialogKeysPure.h; this header only reads ImGui state for them.
//
// Usage, for a confirm: keep a file-scope DialogScopeState next to the dialog, call BeginDialog where the
// BeginPopupModal was, draw the message, then call ConfirmFooter with a spec (DestructiveFooter,
// ConsentFooter or UnsavedChangesFooter cover most dialogs) and act on the pick it returns. EndDialog
// closes the body. "Delete view?" in SmatchetViewsDashboardUi.cpp is a short example.
// Cancel (button or Esc) closes the popup inside ConfirmFooter; Primary and Alternate leave closing to the
// caller, since some actions keep the dialog open when they are refused.
//
// Labels and popup ids go through SmatchetLocalizedImGui, as the call sites' own `ImGui::` calls do under
// their `#define ImGui SmatchetLocalizedImGui`, so ids hash the same and labels translate the same.

#include "Ui/DialogKeysPure.h"
#include "imgui.h"

namespace smatchet {
namespace ui {

/// Per-dialog state, owned by the caller (a file-scope static next to the dialog is the usual home).
struct DialogScopeState {
    bool Open = false;                   ///< BeginDialog returned true last time it was called
    bool FocusPending = false;           ///< the opening focus has not been placed yet
    bool OwnedKeyboardLastFrame = false; ///< for ResolveEscape: the Esc that closed a popup above stays there
    bool FieldActiveLastFrame = false;   ///< a field of the dialog held the caret at the end of last frame
};

/// Wraps BeginPopupModal. Call every frame the dialog could be open, where its BeginPopupModal was. Returns
/// what BeginPopupModal returns; when true, the body runs and EndDialog must follow.
bool BeginDialog(DialogScopeState& s, const char* popupId, bool* pOpen = nullptr, ImGuiWindowFlags flags = 0);

/// Ends the dialog body (EndPopup) and records this frame's keyboard state for the next.
void EndDialog(DialogScopeState& s);

/// Call immediately before the widget that takes the keyboard when the dialog opens. Puts it there once,
/// waiting while the Enter that opened the dialog is still held. Returns true on the frame it does.
bool DialogFocusHere(DialogScopeState& s);

/// The form-level keys of a dialog this frame.
struct DialogKeys {
    dialogkeys::EscapeAction Esc = dialogkeys::EscapeAction::None;
    bool Submit = false; ///< Ctrl+Enter
};

/// Reads a form dialog's Esc and Ctrl+Enter. Call at the top of the body, before any item, so a key the
/// dialog takes does not also reach the field or button that has the keyboard. `nothingTyped`: closing now
/// loses no typed text (a search filter counts as nothing). On LeaveField the field is left here; on Cancel
/// the caller closes the dialog the way its Cancel button does.
DialogKeys DialogReadKeys(DialogScopeState& s, bool nothingTyped);

/// The button row of a confirm dialog: [Primary] [Alternate] [Cancel], action first (P2-M16 order).
struct ConfirmFooterSpec {
    const char* PrimaryLabel = nullptr;   ///< required
    bool PrimaryDestructive = false;      ///< red, and the safe button takes the keyboard on open
    bool PrimaryEnabled = true;           ///< drawn disabled when false; Enter never runs it then
    const char* AlternateLabel = nullptr; ///< optional second action
    bool AlternateDestructive = false;    ///< red
    const char* CancelLabel = nullptr;    ///< nullptr: "Cancel"
    bool SafeByDefault = false;           ///< the safe button takes the keyboard even for a non-destructive action
    ImVec2 ButtonSize = ImVec2(0.0f, 0.0f);
};

/// One action that deletes or replaces something: red, and the keyboard starts on the safe button.
inline ConfirmFooterSpec DestructiveFooter(const char* primaryLabel, const char* cancelLabel = nullptr) {
    ConfirmFooterSpec spec;
    spec.PrimaryLabel = primaryLabel;
    spec.PrimaryDestructive = true;
    spec.CancelLabel = cancelLabel;
    return spec;
}

/// A consent prompt: the action is not destructive, but it widens what leaves the app, so the keyboard
/// starts on the safe button.
inline ConfirmFooterSpec ConsentFooter(const char* primaryLabel) {
    ConfirmFooterSpec spec;
    spec.PrimaryLabel = primaryLabel;
    spec.SafeByDefault = true;
    return spec;
}

/// Unsaved changes: save (the default), discard (red), or go back.
inline ConfirmFooterSpec UnsavedChangesFooter(const char* saveLabel, const char* discardLabel,
                                              const char* cancelLabel = nullptr) {
    ConfirmFooterSpec spec;
    spec.PrimaryLabel = saveLabel;
    spec.AlternateLabel = discardLabel;
    spec.AlternateDestructive = true;
    spec.CancelLabel = cancelLabel;
    return spec;
}

/// Draws the row, places the opening focus on the default button, and maps Enter and Esc. Returns the
/// button the user ran this frame. Cancel closes the popup; Primary and Alternate do not.
dialogkeys::ConfirmPick ConfirmFooter(DialogScopeState& s, const ConfirmFooterSpec& spec);

/// True while any modal popup is open. SmatchetUI::dispatchKeybindings mutes every shortcut but those in
/// dialogkeys::GlobalShortcutAllowedUnderModal while this holds.
bool AnyModalDialogOpen();

} // namespace ui
} // namespace smatchet
