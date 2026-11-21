#include "Ui/SmatchetDialogScope.h"

#include "SmatchetLocalizedImGui.h"
#include "Ui/SmatchetDestructiveButton.h"
#include "imgui_internal.h"

#include <cstring>

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

DialogKeys DialogReadKeys(DialogScopeState& s, bool nothingTyped, bool readSubmit) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool owns = DialogOwnsKeyboard();
    const bool esc = EscPressed();
    DialogKeys keys;
    keys.Esc =
        dialogkeys::ResolveEscape(esc, owns, s.OwnedKeyboardLastFrame, FieldActiveForEsc(s, g, esc), nothingTyped);
    // Gated like Esc: the Ctrl+Enter that opened the dialog must not also submit it.
    keys.Submit = readSubmit && owns && s.OwnedKeyboardLastFrame &&
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
    // Ctrl+Enter is a form's submit, not a confirm's Enter: one aimed at the form under the confirm (the
    // long-text editor's save) must not answer the confirm either.
    const bool plainEnter = EnterPressed() && !ImGui::GetIO().KeyCtrl;
    if (owns && EnterPressed() && ImGui::GetIO().KeyCtrl) {
        // ImGui activates the item under a visible keyboard cursor on Enter whatever the modifiers, so
        // without this Ctrl+Enter would still press the focused button (Discard included).
        g.NavActivateId = 0;
        g.NavActivateDownId = 0;
        g.NavActivatePressedId = 0;
    }
    const ConfirmPick enterPick =
        dialogkeys::ResolveConfirmEnter(plainEnter, owns, s.OwnedKeyboardLastFrame, navItemTakesEnter, def);
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

namespace {

// One tracker for the whole UI: modals stack, and the caret goes back once the last of them closes.
dialogkeys::CaretReturnTracker g_caretReturn;

// Frames a return request survives without the field taking the caret. The activation lands on the frame
// after the request, so two would do; the rest covers a window that skips a frame while its dock settles.
constexpr int kCaretReturnTries = 4;

ImGuiID RootId(const ImGuiWindow* w) { return (w != nullptr && w->RootWindow != nullptr) ? w->RootWindow->ID : 0; }

// The text field holding the caret, 0 for none. ImGui keeps one text-edit state, for the field being edited.
ImGuiID ActiveTextField(const ImGuiContext& g) {
    return (g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId) ? g.ActiveId : 0;
}

// Where the caret and selection were in a text field. ImGui keeps one text-edit state, for the field being
// edited, so a field that lost the caret to another field (the palette's filter, a dialog's own field) is
// set up afresh when it is activated again, with all its text selected: the next key would replace it.
struct CaretSpot {
    ImGuiID Field = 0;
    int Anchor = 0;
    int Cursor = 0;
};

CaretSpot g_fieldCaret;   // the field that had the caret, as of the last frame no modal was up
CaretSpot g_openerCaret;  // the field the palette was opened from
CaretSpot g_restoreCaret; // to put back once the returned field has the caret again
int g_restoreFrames = 0;

CaretSpot ReadCaret(const ImGuiContext& g, ImGuiID field) {
    CaretSpot spot;
    if (field == 0 || g.InputTextState.ID != field) {
        return spot;
    }
    const ImGuiInputTextState& st = g.InputTextState;
    spot.Field = field;
    spot.Anchor = st.HasSelection() ? st.GetSelectionStart() : st.GetCursorPos();
    spot.Cursor = st.HasSelection() ? st.GetSelectionEnd() : st.GetCursorPos();
    return spot;
}

// Puts an owed caret back on the first frame its field has the caret; drops it after a few frames.
void RestoreOwedCaret(ImGuiContext& g, ImGuiID activeField) {
    if (g_restoreCaret.Field == 0) {
        return;
    }
    if (activeField == g_restoreCaret.Field) {
        g.InputTextState.SetSelection(g_restoreCaret.Anchor, g_restoreCaret.Cursor);
        g.InputTextState.CursorClamp();
        g.InputTextState.CursorFollow = true;
        g_restoreCaret = CaretSpot();
    } else if (--g_restoreFrames <= 0) {
        g_restoreCaret = CaretSpot();
    }
}

} // namespace

void ServiceDialogFocusReturn() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiID field = ActiveTextField(g);
    RestoreOwedCaret(g, field);
    const bool modalUp = AnyModalDialogOpen();
    if (field != 0 && !modalUp) {
        g_fieldCaret = ReadCaret(g, field);
    }
    const unsigned int give = dialogkeys::StepCaretReturn(
        g_caretReturn, modalUp, field, field != 0 ? RootId(g.ActiveIdWindow) : 0, RootId(g.NavWindow),
        ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter), kCaretReturnTries);
    if (give != 0) {
        // Keep the caret where it was, as clicking back into the field does, instead of selecting the text.
        ImGui::ActivateItemByID(give);
        g.NavNextActivateFlags = ImGuiActivateFlags_PreferInput | ImGuiActivateFlags_TryToPreserveState;
        // TryToPreserveState only holds while ImGui's one text state is still this field's: put the caret and
        // selection back by hand once the field is active, in case it was not.
        if (g_fieldCaret.Field == give) {
            g_restoreCaret = g_fieldCaret;
        } else if (g_openerCaret.Field == give) {
            g_restoreCaret = g_openerCaret;
        } else {
            g_restoreCaret = CaretSpot();
        }
        g_restoreFrames = kCaretReturnTries + 1;
    }
}

FocusOpener CaptureFocusOpener(const char* ignoreRootName) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    FocusOpener opener;
    const ImGuiWindow* nav = g.NavWindow != nullptr ? g.NavWindow->RootWindow : nullptr;
    if (nav == nullptr || (ignoreRootName != nullptr && std::strcmp(nav->Name, ignoreRootName) == 0)) {
        return opener;
    }
    opener.Window = nav->ID;
    const ImGuiID field = ActiveTextField(g);
    if (field != 0 && RootId(g.ActiveIdWindow) == nav->ID) {
        opener.TextField = field;
        g_openerCaret = ReadCaret(g, field);
    }
    return opener;
}

void ReturnFocusTo(const FocusOpener& opener, const char* onlyFromRootName) {
    ImGuiWindow* w = opener.Window != 0 ? ImGui::FindWindowByID(opener.Window) : nullptr;
    if (w == nullptr || !w->WasActive) {
        return;
    }
    const ImGuiWindow* nav = GImGui->NavWindow != nullptr ? GImGui->NavWindow->RootWindow : nullptr;
    if (onlyFromRootName != nullptr && nav != nullptr && std::strcmp(nav->Name, onlyFromRootName) != 0) {
        return;
    }
    // Also takes the caret out of the overlay's own field: FocusWindow drops an active item of another root.
    ImGui::FocusWindow(w, ImGuiFocusRequestFlags_RestoreFocusedChild);
    if (opener.TextField != 0) {
        dialogkeys::RequestCaretReturn(g_caretReturn, opener.TextField, opener.Window);
    }
}

} // namespace ui
} // namespace smatchet
