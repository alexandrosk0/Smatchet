#ifndef SMATCHET_UI_SMATCHET_DESTRUCTIVE_BUTTON_H
#define SMATCHET_UI_SMATCHET_DESTRUCTIVE_BUTTON_H

// One destructive-red button style for every confirm dialog (P2-M16): the delete-view
// confirm, the delete-view header button, and the DB-recreate confirm previously each
// carried their own PushStyleColor triple (two of them with drifting reds).
// Deliberately ::ImGui — style pushes carry no translatable content; call sites keep
// their own (possibly aliased/translated) Button call between Push and Pop.

#include "imgui.h"

inline void SmatchetPushDestructiveButtonColors() {
    // SMATCHET_DEVIATION(rule=duplication; reason=style-push triple shape; owner=ui; revisit=dup-scoping)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
}

inline void SmatchetPopDestructiveButtonColors() { ImGui::PopStyleColor(3); }

#endif // SMATCHET_UI_SMATCHET_DESTRUCTIVE_BUTTON_H
