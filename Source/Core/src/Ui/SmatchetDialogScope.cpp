#include "Ui/SmatchetDialogScope.h"

#include "SmatchetLocalizedImGui.h"
#include "Ui/SmatchetDestructiveButton.h"
#include "imgui_internal.h"

namespace smatchet {
namespace ui {

namespace {

bool DialogOwnsKeyboard() {
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy);
}

// A field inside the current dialog (any child of its root window) holds the caret.
bool FieldActiveInDialog(const ImGuiContext& g) {
    return g.ActiveId != 0 && g.ActiveIdWindow != nullptr && g.CurrentWindow != nullptr &&
           g.ActiveIdWindow->RootWindow == g.CurrentWindow->RootWindow;
}

bool EscPressed() { return ImGui::IsKeyPressed(ImGuiKey_Escape, false); }

bool EnterPressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
}

// The field that held the caret last frame still counts on the Esc frame: ImGui's keyboard-nav cancel
// clears the active item before any window draws, so without this the first Esc would always read as
// "nothing being typed in" and close the dialog around typed text.
bool FieldActiveForEsc(const DialogScopeState& s, const ImGuiContext& g, bool escPressed) {
    return FieldActiveInDialog(g) || (escPressed && s.FieldActiveLastFrame);
}

} // namespace

bool BeginDialog(DialogScopeState& s, const char* popupId, bool* pOpen, ImGuiWindowFlags flags) {
    if (!SmatchetLocalizedImGui::BeginPopupModal(popupId, pOpen, flags)) {
        s = DialogScopeState();
        return false;
    }
    // IsWindowAppearing also catches a re-open the state missed: a dialog whose BeginDialog is skipped on
    // some frames (drawn only while its owner shows) can be closed and opened again in between.
    if (!s.Open || ImGui::IsWindowAppearing()) {
        s = DialogScopeState();
        s.Open = true;
        s.FocusPending = true;
    }
    return true;
}

void EndDialog(DialogScopeState& s) {
    s.OwnedKeyboardLastFrame = DialogOwnsKeyboard();
    s.FieldActiveLastFrame = FieldActiveInDialog(*ImGui::GetCurrentContext());
    ImGui::EndPopup();
}

bool DialogFocusHere(DialogScopeState& s) {
    if (!s.FocusPending) {
        return false;
    }
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const dialogkeys::CaretRequest request =
        dialogkeys::ResolveCaretRequest(ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter),
                                        g.MovingWindow != nullptr || ImGui::IsDragDropActive(), DialogOwnsKeyboard());
    if (request == dialogkeys::CaretRequest::Wait) {
        return false;
    }
    s.FocusPending = false;
    if (request != dialogkeys::CaretRequest::Take) {
        return false;
    }
    ImGui::SetKeyboardFocusHere();
    return true;
}

DialogKeys DialogReadKeys(DialogScopeState& s, bool nothingTyped) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool owns = DialogOwnsKeyboard();
    const bool esc = EscPressed();
    DialogKeys keys;
    keys.Esc =
        dialogkeys::ResolveEscape(esc, owns, s.OwnedKeyboardLastFrame, FieldActiveForEsc(s, g, esc), nothingTyped);
    // Gated like Esc: the Ctrl+Enter that opened the dialog must not also submit it.
    keys.Submit = owns && s.OwnedKeyboardLastFrame &&
                  (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter) ||
                   ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter));
    if (FieldActiveInDialog(g) && (keys.Submit || keys.Esc != dialogkeys::EscapeAction::None)) {
        // Leave the field the way clicking away does: its text stays. ImGui's own Esc in a field would put
        // back the text from before the caret entered.
        ImGui::ClearActiveID();
    }
    if (keys.Submit) {
        // ImGui activates the item under the keyboard cursor on Enter whatever the modifiers: Ctrl+Enter on
        // Cancel would otherwise both submit and cancel.
        g.NavActivateId = 0;
        g.NavActivateDownId = 0;
        g.NavActivatePressedId = 0;
    }
    return keys;
}

dialogkeys::ConfirmPick ConfirmFooter(DialogScopeState& s, const ConfirmFooterSpec& spec) {
    using dialogkeys::ConfirmDefault;
    using dialogkeys::ConfirmPick;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool owns = DialogOwnsKeyboard();
    const bool esc = EscPressed();
    const ConfirmDefault def =
        dialogkeys::ResolveConfirmDefault(spec.PrimaryDestructive, spec.SafeByDefault || !spec.PrimaryEnabled);

    // Read before the buttons draw. ImGui activates the item under a visible keyboard cursor on Enter at the
    // start of the frame (the same test as its NavUpdate), so that item reports the click below and Enter
    // must not also pick the default.
    const bool navItemTakesEnter =
        g.NavId != 0 && g.NavCursorVisible && g.NavWindow != nullptr && g.CurrentWindow != nullptr &&
        g.NavWindow->RootWindow == g.CurrentWindow->RootWindow && (g.ActiveId == 0 || g.ActiveId == g.NavId);
    const ConfirmPick enterPick =
        dialogkeys::ResolveConfirmEnter(EnterPressed(), owns, s.OwnedKeyboardLastFrame, navItemTakesEnter, def);
    const dialogkeys::EscapeAction escape =
        dialogkeys::ResolveEscape(esc, owns, s.OwnedKeyboardLastFrame, FieldActiveForEsc(s, g, esc), true);

    const auto drawButton = [&](const char* label, bool destructive, bool enabled, bool takesFocus) {
        if (takesFocus) {
            DialogFocusHere(s);
        }
        ImGui::BeginDisabled(!enabled);
        if (destructive) {
            SmatchetPushDestructiveButtonColors();
        }
        const bool clicked = SmatchetLocalizedImGui::Button(label, spec.ButtonSize);
        if (destructive) {
            SmatchetPopDestructiveButtonColors();
        }
        ImGui::EndDisabled();
        return clicked && enabled;
    };

    ConfirmPick pick = ConfirmPick::None;
    if (drawButton(spec.PrimaryLabel, spec.PrimaryDestructive, spec.PrimaryEnabled, def == ConfirmDefault::Primary)) {
        pick = ConfirmPick::Primary;
    }
    if (spec.AlternateLabel != nullptr) {
        ImGui::SameLine();
        if (drawButton(spec.AlternateLabel, spec.AlternateDestructive, true, false)) {
            pick = ConfirmPick::Alternate;
        }
    }
    ImGui::SameLine();
    if (drawButton(spec.CancelLabel != nullptr ? spec.CancelLabel : "Cancel", false, true,
                   def == ConfirmDefault::Safe)) {
        pick = ConfirmPick::Cancel;
    }

    if (pick == ConfirmPick::None) {
        pick = enterPick;
    }
    if (pick == ConfirmPick::None && escape == dialogkeys::EscapeAction::Cancel) {
        pick = ConfirmPick::Cancel;
    } else if (escape == dialogkeys::EscapeAction::LeaveField && FieldActiveInDialog(g)) {
        ImGui::ClearActiveID();
    }
    if (pick == ConfirmPick::Primary && !spec.PrimaryEnabled) {
        pick = ConfirmPick::None;
    }
    if (pick == ConfirmPick::Cancel) {
        ImGui::CloseCurrentPopup();
    }
    return pick;
}

bool AnyModalDialogOpen() { return ImGui::GetTopMostPopupModal() != nullptr; }

} // namespace ui
} // namespace smatchet
