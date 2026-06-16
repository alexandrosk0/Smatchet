#include "SmatchetLocalizedImGui.h"

// Out-of-line definition of the dictation focus-hook, moved off the hot,
// ~45-includer SmatchetLocalizedImGui.h header (debt 2026-05-18). The three routed
// InputText wrappers in that header call this once per draw; keeping the body here
// means the ImGui item-state queries (IsItemActive / IsItemFocused / GetItemID) and
// the router splice compile in one TU instead of every TU that includes the header.
// Behaviour is byte-for-byte identical to the prior inline body.

namespace SmatchetLocalizedImGui {

void HookDictationOnLastItem(char* buf, std::size_t buf_size) {
    const bool active = ::ImGui::IsItemActive() || ::ImGui::IsItemFocused();
    if (active) {
        // Capture the just-drawn widget's ImGui id so the router can pick the
        // correct entry from multiple registered surfaces when
        // `InsertIntoFocusedInputText(text, ImGui::GetActiveID())` runs.
        // ImGuiID is `unsigned int`; the router takes it through a plain
        // `unsigned int` parameter to keep imgui.h out of the router header.
        const ImGuiID itemId = ::ImGui::GetItemID();
        g_dictationRouter.RegisterInputTextWithItemId(buf, buf_size, nullptr, static_cast<unsigned int>(itemId));
    } else {
        g_dictationRouter.UnregisterInputText(buf);
    }
}

}  // namespace SmatchetLocalizedImGui
