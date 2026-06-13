#include "Ui/SmatchetTextureFaultInjector.h"

#if defined(SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION)

#include "imgui.h"

namespace {
// Negative-control flag the #1133 scenario reads to label a sub-step. It does NOT disable the
// production guard — the test asserts the guard STILL repoints when rearm is "disabled"; the
// injector merely records the bool the scenario set.
bool g_rearmDisabled = false;

// A distinct, retired ImTextureData that stands in for the OLD atlas a mid-frame grow tore down
// (#1122). It must NOT be the live font atlas: GuardImGuiDynamicTextures path-2 skips any command
// whose texture IS the live atlas (it is the repoint TARGET), so dooming the live atlas can never
// trigger the repoint+LOG_WARN — the original #1133 green-wash. It is function-local-static (not a
// stack temp) because the ImDrawCmd we commit holds a RAW ImTextureData* (ImTextureRef::_TexData)
// that must outlive ImGui::Render() AND the end-of-frame guard that reads it.
//
// Doomed state must satisfy SmatchetDrawCmdTextureNeedsRebind(status, hasPixels, texIdInvalid):
// TexID Invalid AND not (WantCreate && hasPixels). We keep Pixels == NULL so SetStatus(Destroyed)
// STICKS — ImTextureData::SetStatus auto-promotes Destroyed->WantCreate when Pixels != NULL (the
// self-heal), which would make the command "safe" (WantCreate+pixels) and defeat the repoint. A
// default-constructed ImTextureData is already {Status=Destroyed, TexID=Invalid, Pixels=NULL} —
// exactly the doomed orphan; we re-assert the state each call so a prior frame can't leave it OK.
ImTextureData& RetiredOrphanTexture() {
    static ImTextureData s_orphan; // raw ptr captured by the committed ImDrawCmd; must outlive Render()
    s_orphan.SetTexID(ImTextureID_Invalid);
    s_orphan.SetStatus(ImTextureStatus_Destroyed); // sticks: Pixels==NULL so no auto-promote to WantCreate
    return s_orphan;
}
} // namespace

// ORDER MATTERS (architect open-Q3): the AddImage record must precede the SetStatus/SetTexID doom so
// ImGui does not re-validate the texture at record time — the committed command then lands on the
// doomed texture and survives to ImGui::Render(), reproducing the orphaned-command fault the guard
// must repoint. The doomed texture is a DISTINCT retired texture (RetiredOrphanTexture), never the
// live atlas, so the guard's path-2 repoint actually fires (and logs).
void smatchet::ui::ForceTextureStuck() {
    ImTextureData& orphan = RetiredOrphanTexture();
    ImGui::GetForegroundDrawList()->AddImage(orphan.GetTexRef(), ImVec2(0, 0), ImVec2(1, 1)); // cmd on the retired orphan
    orphan.SetTexID(ImTextureID_Invalid); // re-assert doom AFTER recording (record-time must not re-validate)
    orphan.SetStatus(ImTextureStatus_Destroyed);
}

void smatchet::ui::ForceContextLoss() {
    // Same orphan shape as ForceTextureStuck: a committed command on a DISTINCT retired texture the
    // guard's path-2 repoints + logs. (Path-1's in-list Destroyed-with-pixels re-arm does NOT log, so
    // a pixels-intact live-atlas doom can never satisfy the scenario's path-2 LOG_WARN scan — that was
    // the second #1133 green-wash. The "context loss" label is retained for the scenario's sub-step
    // taxonomy; both sub-steps exercise the same orphaned-command repoint the LOG_WARN reports.)
    ImTextureData& orphan = RetiredOrphanTexture();
    ImGui::GetForegroundDrawList()->AddImage(orphan.GetTexRef(), ImVec2(0, 0), ImVec2(1, 1));
    orphan.SetTexID(ImTextureID_Invalid);
    orphan.SetStatus(ImTextureStatus_Destroyed);
}

void smatchet::ui::SetRearmDisabled(bool disabled) {
    g_rearmDisabled = disabled;
}

bool smatchet::ui::RearmDisabled() {
    return g_rearmDisabled;
}

#endif // SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION
