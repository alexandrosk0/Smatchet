#ifndef SMATCHET_UI_SMATCHET_SECRET_INPUT_H
#define SMATCHET_UI_SMATCHET_SECRET_INPUT_H

// Shared masked-credential input (P2-L11): a password InputText with a per-field
// Show/Hide reveal toggle, a character count, and a leading/trailing-whitespace
// warning. The count + whitespace cue exist because an invisible trailing space in
// a pasted token once 401'd every Jira request (issue #979) — with a masked field
// there was nothing to see. Used by every credential field in Preferences
// (Tracker / Integrations / Assistant / Whisper).
//
// Deliberately ::ImGui (not the localization alias): included above the TU's
// `#define ImGui SmatchetLocalizedImGui` line, and secret fields carry no
// translatable content. Reveal state lives in the window's ImGui state storage,
// keyed off the widget id, so each field toggles independently.

#include "imgui.h"

#include <cstring>

/// Draws `label` as a masked text input + "Show" toggle + length/whitespace hints.
/// Returns true when the text was edited this frame (same contract as InputText).
/// `outEditing` (optional) receives whether the inner InputText still holds keyboard focus, i.e.
/// whether `buf` currently holds a half-typed PREFIX. It has to be reported from in here: the
/// widget is compound and ends on the Show/Hide SmallButton or the char-count TextDisabled, so a
/// caller's own ImGui::IsItemActive() would read one of those, not the input (#2110). Callers that
/// act on the buffer immediately — rather than behind a Save button — must gate on it.
inline bool SmatchetSecretInputText(const char* label, char* buf, std::size_t bufSize, bool* outEditing = nullptr) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID shownId = ImGui::GetID(label) + 1u; // offset: distinct from the input's own id
    const bool shown = storage->GetBool(shownId, false);
    const bool edited =
        ImGui::InputText(label, buf, bufSize, shown ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password);
    if (outEditing != nullptr) {
        *outEditing = ImGui::IsItemActive(); // must be sampled here, before the SameLine chain below
    }
    ImGui::SameLine();
    ImGui::PushID(label);
    if (ImGui::SmallButton(shown ? "Hide" : "Show")) {
        storage->SetBool(shownId, !shown);
    }
    ImGui::PopID();
    const std::size_t len = std::strlen(buf);
    if (len > 0u) {
        const bool edgeWhitespace = buf[0] == ' ' || buf[0] == '\t' || buf[len - 1u] == ' ' || buf[len - 1u] == '\t';
        ImGui::SameLine();
        if (edgeWhitespace) {
            ImGui::TextDisabled("(%d chars, has leading/trailing whitespace)", static_cast<int>(len));
        } else {
            ImGui::TextDisabled("(%d chars)", static_cast<int>(len));
        }
    }
    return edited;
}

#endif // SMATCHET_UI_SMATCHET_SECRET_INPUT_H
