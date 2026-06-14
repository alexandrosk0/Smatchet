#pragma once

namespace smatchet {
namespace ui {

#if defined(SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION)
// All called from INSIDE a live ImGui frame (scenario OnFrame, mid-Draw), so a
// committed draw command lands on the doomed texture before ImGui::Render().
void ForceTextureStuck();   // commit a cmd on the live atlas, then flip it Destroyed + TexID Invalid (no pixels) -> orphaned committed cmd
void ForceContextLoss();    // flip a font ImTextureData to Destroyed while Pixels intact + TexID Invalid (ctx-loss re-arm analogue)
void SetRearmDisabled(bool disabled);
bool RearmDisabled();
#else
inline void ForceTextureStuck() {}
inline void ForceContextLoss() {}
inline void SetRearmDisabled(bool) {}
inline bool RearmDisabled() { return false; }
#endif

} // namespace ui
} // namespace smatchet
