#include "imgui.h"

#include "Logger.h"
#include "Ui/SmatchetImGuiTextureGuard.h"
#include "Ui/SmatchetImGuiTextureGuardRuntime.h"

// Pin the pure guard's status constants to ImGui's enum so the predicate (compiled into the Windows
// doctest rig, which has no imgui.h) can never silently drift from the values this TU reasons about.
static_assert(static_cast<int>(smatchet::ui::kTextureGuardStatus_OK) == static_cast<int>(ImTextureStatus_OK) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_Destroyed) ==
                      static_cast<int>(ImTextureStatus_Destroyed) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_WantCreate) ==
                      static_cast<int>(ImTextureStatus_WantCreate) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_WantUpdates) ==
                      static_cast<int>(ImTextureStatus_WantUpdates) &&
                  static_cast<int>(smatchet::ui::kTextureGuardStatus_WantDestroy) ==
                      static_cast<int>(ImTextureStatus_WantDestroy),
              "SmatchetImGuiTextureGuard status constants drifted from ImGui's ImTextureStatus enum");

namespace {
// File-scope so a test harness can re-arm it via ResetTextureGuardLogLatch() between forced-fault
// sub-steps. Latches the once-per-episode "[P0 #1122] repointed" warning.
bool g_textureGuardLogged = false;
} // namespace

// [P0 #1122] ImGui 1.92 dynamic-texture safety pass. Runs after ImGui::Render() and before the
// backend reads any draw command's texture id, now that ImGui 1.92 owns texture lifetime through the
// ImTextureData type. Two paths can leave a committed ImDrawCmd pointing at a texture whose backend
// TexID is Invalid when the command loop reads it, which ImDrawCmd::GetTexID() asserts on (SIGABRT,
// "Backend must call
// ImTextureData::SetTexID()..."):
//   (1) Context-loss re-arm (list-walk). After an EGL/activity recreate (RecreateAfterContextLoss,
//       INIT_WINDOW re-init, background->foreground) a font texture can reach render Destroyed with
//       TexID=Invalid: the core's Destroyed->WantCreate self-heal is skipped when WantDestroyNextFrame
//       is set, and the GL3 backend's UpdateTexture has no Destroyed branch. If its CPU pixels survive,
//       force WantCreate so the backend re-uploads it during its Textures[] pass — which runs in the
//       same RenderDrawData call, before the draw commands sample the texture. (Validated on device via
//       the forced EGL_CONTEXT_LOST path.)
//   (2) Orphaned-command repoint (command-walk). A mid-frame dynamic-atlas grow / font re-apply
//       (on-demand glyph baking when a new tab/label first renders) retires the OLD atlas texture while
//       a command was already committed on it this frame; by the command loop that texture's id is
//       Invalid (destroyed in the Textures[] pass, or removed from the texture list entirely). Path (1)
//       only walks drawData->Textures and never the per-command TexRef, so it structurally misses an
//       orphan that is no longer in the list. Walk the committed commands and repoint any doomed one
//       (SmatchetDrawCmdTextureNeedsRebind: an Invalid id the upload pass will NOT validate this frame)
//       at the live atlas texture, which that pass makes valid. The walk is over draw COMMANDS (tens to
//       hundreds per frame, not vertices), so the per-frame cost is negligible (Quality Pillar 1).
// A repoint changes only the command's TexRef; its vertices keep their old UVs, so the worst case is
// one frame of cosmetically-wrong glyphs on a rare transition — never a crash. Confirmed on a physical
// Pixel for the rotate->dock-tab->tap-Tickets repro (#1122). Shared by the GL3 standalone render loop
// and the Android render loop (promoted out of android_main.cpp for #1133 fault-injection coverage).
void smatchet::ui::GuardImGuiDynamicTextures(ImDrawData* drawData) {
    if (drawData == nullptr) {
        return;
    }
    // (1) context-loss re-arm: resurrect any in-list texture left Invalid with its CPU pixels intact.
    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex != nullptr && tex->GetTexID() == ImTextureID_Invalid &&
                tex->Status != ImTextureStatus_WantCreate && tex->Pixels != nullptr) {
                tex->SetStatus(ImTextureStatus_WantCreate); // backend re-uploads it this same frame
            }
        }
    }
    // (2) orphaned-command repoint: repoint any committed command whose texture the upload pass will
    // leave Invalid onto the live atlas (which that same pass makes valid this frame).
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImTextureData* liveAtlas = atlas != nullptr ? atlas->TexData : nullptr;
    if (liveAtlas == nullptr) {
        return;
    }
    int repointed = 0;
    for (int li = 0; li < drawData->CmdListsCount; ++li) {
        ImDrawList* dl = drawData->CmdLists[li];
        if (dl == nullptr) {
            continue;
        }
        for (ImDrawCmd& cmd : dl->CmdBuffer) {
            ImTextureData* td = cmd.TexRef._TexData;
            if (td == nullptr || td == liveAtlas) {
                continue; // no per-texture id to resolve, or already the live atlas the pass validates
            }
            if (!smatchet::ui::SmatchetDrawCmdTextureNeedsRebind(
                    static_cast<int>(td->Status), td->Pixels != nullptr,
                    td->GetTexID() == ImTextureID_Invalid)) {
                continue;
            }
            cmd.TexRef = liveAtlas->GetTexRef(); // GetTexRef() (not ImTextureRef(td)) wires _TexData
            ++repointed;
        }
    }
    if (repointed > 0) {
        if (!g_textureGuardLogged) {
            g_textureGuardLogged = true;
            LOG_WARN("[P0 #1122] repointed %d orphaned draw command(s) to the live font atlas after a "
                     "mid-frame atlas teardown (further occurrences silenced)",
                     repointed);
        }
    }
}

void smatchet::ui::ResetTextureGuardLogLatch() {
    g_textureGuardLogged = false;
}
