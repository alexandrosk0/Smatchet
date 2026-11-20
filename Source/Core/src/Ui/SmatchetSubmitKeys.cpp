// Submit-key widgets: see Ui/SmatchetSubmitKeys.h for the contract and Ui/SubmitKeysPure.h for the
// layout rules.

#include "Ui/SmatchetSubmitKeys.h"

#include "SmatchetLocalization.h"
#include "SmatchetLocalizedImGui.h"
#include "Ui/SubmitKeysPure.h"

#if defined(SMATCHET_BUILD_UI_TESTS)
#include <string>
#include <unordered_map>
#endif

namespace smatchet {
namespace ui {

namespace {

#if defined(SMATCHET_BUILD_UI_TESTS)
// What was last drawn for an item, and on which frame. Test builds only, so the shipping frame
// path keeps no per-item bookkeeping.
struct DrawnForTest {
    std::string Text;
    int Frame = -1;
};
// Keyed by the caller's label: a test cannot easily rebuild the id of a widget deep in a popup.
std::unordered_map<std::string, DrawnForTest>& KeyHintsDrawn() {
    static std::unordered_map<std::string, DrawnForTest> drawn;
    return drawn;
}
std::unordered_map<std::string, DrawnForTest>& PlaceholdersDrawn() {
    static std::unordered_map<std::string, DrawnForTest> drawn;
    return drawn;
}
// A test reads between frames, so "drawn" means on the current frame or the one before it.
bool DrawnRecently(const DrawnForTest& d) { return d.Frame >= 0 && ::ImGui::GetFrameCount() - d.Frame <= 1; }
#endif

// Dim text inside the last item's frame, clipped to it.
void DrawDimTextInLastItem(const ImVec2& pos, const char* text, float wrapWidth) {
    const ImVec2 min = ::ImGui::GetItemRectMin();
    const ImVec2 max = ::ImGui::GetItemRectMax();
    const ImVec4 clip(min.x, min.y, max.x, max.y);
    ::ImGui::GetWindowDrawList()->AddText(::ImGui::GetFont(), ::ImGui::GetFontSize(), pos,
                                          ::ImGui::GetColorU32(ImGuiCol_TextDisabled), text, nullptr, wrapWidth, &clip);
}

} // namespace

bool SubmitMultilineInput(const char* label, char* buf, std::size_t bufSize, const ImVec2& size,
                          const char* placeholder, ImGuiInputTextFlags extraFlags, bool dictation) {
    if (buf == nullptr || bufSize == 0) {
        return false;
    }
    const ImGuiInputTextFlags flags =
        (extraFlags & ~(ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CtrlEnterForNewLine)) |
        ImGuiInputTextFlags_EnterReturnsTrue;
    // The localized wrapper translates the label and registers the buffer for dictation; without
    // dictation only the label is translated.
    const bool submitted = dictation ? SmatchetLocalizedImGui::InputTextMultiline(label, buf, bufSize, size, flags)
                                     : ::ImGui::InputTextMultiline(SmatchetLocalization::LabelFromSource(label), buf,
                                                                   bufSize, size, flags);
    if (placeholder != nullptr && placeholder[0] != '\0' && submitkeys::ShowsPlaceholder(buf)) {
        const ImGuiStyle& style = ::ImGui::GetStyle();
        const ImVec2 min = ::ImGui::GetItemRectMin();
        const ImVec2 max = ::ImGui::GetItemRectMax();
        const float wrap = max.x - min.x - style.FramePadding.x * 2.0f;
        DrawDimTextInLastItem(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y), placeholder,
                              wrap > 0.0f ? wrap : 0.0f);
#if defined(SMATCHET_BUILD_UI_TESTS)
        DrawnForTest& d = PlaceholdersDrawn()[label != nullptr ? label : ""];
        d.Text = placeholder;
        d.Frame = ::ImGui::GetFrameCount();
#endif
    }
    return submitted;
}

bool KeyHintButton(const char* label, const char* keyHint, const ImVec2& size) {
    const char* shown = SmatchetLocalization::LabelFromSource(label);
    if (keyHint == nullptr || keyHint[0] == '\0') {
        return ::ImGui::Button(shown, size);
    }
    const ImGuiStyle& style = ::ImGui::GetStyle();
    const float labelWidth = ::ImGui::CalcTextSize(shown, nullptr, /*hide_text_after_double_hash=*/true).x;
    const float hintWidth = ::ImGui::CalcTextSize(keyHint).x;
    // The gap a menu item leaves between its label and its shortcut, scaled with the font.
    const float gap = ::ImGui::GetFontSize();
    const submitkeys::KeyHintButtonLayout layout =
        submitkeys::ComputeKeyHintButtonLayout(labelWidth, hintWidth, gap, style.FramePadding.x, size.x);
    ::ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, style.ButtonTextAlign.y));
    const bool pressed = ::ImGui::Button(shown, ImVec2(layout.Width, size.y));
    ::ImGui::PopStyleVar();
    const ImVec2 min = ::ImGui::GetItemRectMin();
    const ImVec2 max = ::ImGui::GetItemRectMax();
    const float y = min.y + (max.y - min.y - ::ImGui::GetTextLineHeight()) * 0.5f;
    DrawDimTextInLastItem(ImVec2(min.x + layout.HintX, y), keyHint, 0.0f);
#if defined(SMATCHET_BUILD_UI_TESTS)
    DrawnForTest& d = KeyHintsDrawn()[label != nullptr ? label : ""];
    d.Text = keyHint;
    d.Frame = ::ImGui::GetFrameCount();
#endif
    return pressed;
}

const char* CtrlEnterKeyName() { return SmatchetLocalization::T("keys.ctrl_enter", "Ctrl+Enter"); }

#if defined(SMATCHET_BUILD_UI_TESTS)
std::string KeyHintDrawnForTest(const char* buttonLabel) {
    const auto it = KeyHintsDrawn().find(buttonLabel != nullptr ? buttonLabel : "");
    return it != KeyHintsDrawn().end() && DrawnRecently(it->second) ? it->second.Text : std::string();
}

bool PlaceholderDrawnForTest(const char* fieldLabel) {
    const auto it = PlaceholdersDrawn().find(fieldLabel != nullptr ? fieldLabel : "");
    return it != PlaceholdersDrawn().end() && DrawnRecently(it->second);
}
#endif

} // namespace ui
} // namespace smatchet
