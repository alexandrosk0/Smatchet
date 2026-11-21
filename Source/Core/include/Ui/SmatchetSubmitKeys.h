#pragma once

// Submit keys you can see (INPUT_UX_AUDIT.md § 4.4): one multiline field whose Ctrl+Enter submits,
// and one button that prints its key inside the frame, the way a menu item prints its shortcut.
// A composer draws the field, reads its return value as "submit", and draws its submit button with
// CtrlEnterKeyName(); the key and the label that names it then cannot drift apart.
//
// Layout and placeholder rules: Ui/SubmitKeysPure.h. Labels go through the localized ImGui wrapper,
// and the field keeps the wrapper's dictation hook, so a field moved onto this helper still takes
// dictated text. UI-thread only.

#include "imgui.h"

#include <cstddef>
#if defined(SMATCHET_BUILD_UI_TESTS)
#include <string>
#endif

namespace smatchet {
namespace ui {

/// A multiline text field with the § 3 composer keys: Enter types a new line, Ctrl+Enter (main or
/// keypad) submits, Tab moves to the next control. Returns true on the frame Ctrl+Enter was pressed
/// with the caret in the field (or the gamepad validate); ImGui takes the caret out of the field on
/// that frame. `placeholder` (optional) is drawn dim inside the box while it holds no text.
/// `extraFlags` passes through, except ImGuiInputTextFlags_AllowTabInput and
/// ImGuiInputTextFlags_CtrlEnterForNewLine, which would break those keys and are dropped. A null
/// `buf` or a zero `bufSize` draws nothing and returns false.
/// `dictation` registers the buffer with the dictation router, as every localized InputText does. The
/// router keeps a pointer to the last focused buffer past blur, so pass false for a buffer that is not
/// process-static (one owned by an object that can be destroyed, such as across an Unreal hot reload).
bool SubmitMultilineInput(const char* label, char* buf, std::size_t bufSize, const ImVec2& size,
                          const char* placeholder, ImGuiInputTextFlags extraFlags = 0, bool dictation = true);

/// A button that prints `keyHint` dim at its right edge, inside the frame. The label sits at the
/// left; the button widens to fit both, and `size.x` is a minimum width (0 = fit). The id comes from
/// `label` alone, as for ImGui::Button, so the hint never changes which button a caller or a test
/// finds. A null or empty `keyHint` draws a plain button. Returns true on click.
bool KeyHintButton(const char* label, const char* keyHint, const ImVec2& size = ImVec2(0.0f, 0.0f));

/// The localized name of the composer submit key ("Ctrl+Enter").
const char* CtrlEnterKeyName();

#if defined(SMATCHET_BUILD_UI_TESTS)
/// Bucket-E seams: the key name and the placeholder are draw-list text, not items, so a test asks what
/// the current or the previous frame drew for a widget, by the label the caller passed (the key-hint
/// button's label, the field's label). Empty / false when nothing was drawn for it.
std::string KeyHintDrawnForTest(const char* buttonLabel);
bool PlaceholderDrawnForTest(const char* fieldLabel);
#endif

} // namespace ui
} // namespace smatchet
