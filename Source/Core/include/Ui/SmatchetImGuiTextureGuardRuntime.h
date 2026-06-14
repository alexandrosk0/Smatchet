#pragma once

struct ImDrawData; // global-namespace forward decl, matches imgui's own

namespace smatchet {
namespace ui {

// Runs after ImGui::Render(), before the backend reads draw-command texture ids.
// Re-arms in-list textures left Invalid-with-pixels and repoints orphaned commands
// onto the live font atlas. Backend-agnostic (pure ImGui); shared by the GL3
// standalone render loop and the Android render loop. Emits a once-latched
// LOG_WARN "[P0 #1122] repointed N ..." when it repoints.
void GuardImGuiDynamicTextures(ImDrawData* drawData);

// Resets the once-per-episode log latch so a test harness can re-arm the
// "[P0 #1122] repointed" line between forced-fault sub-steps. Harmless in ship
// builds (just resets a file-scope bool).
void ResetTextureGuardLogLatch();

} // namespace ui
} // namespace smatchet
