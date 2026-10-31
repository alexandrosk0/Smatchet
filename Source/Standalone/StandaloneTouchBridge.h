#ifndef SMATCHET_STANDALONE_STANDALONETOUCHBRIDGE_H
#define SMATCHET_STANDALONE_STANDALONETOUCHBRIDGE_H

// Standalone input bridges installed on the GLFW window after ImGui_ImplGlfw_Init*: the keypad-Enter
// key bridge, plus the touchscreen bridge that hands finger input to the ImGui context's
// TouchInputRouter (docs/plans/touchscreen-support.md) instead of to ImGui directly.
//
// GLFW cannot tell a finger from a mouse; the ImGui GLFW backend's WndProc hook latches the source
// of every Windows mouse message (GetMessageExtraInfo) into the context just before GLFW runs the
// callback, so the bridge reads that latch. Mouse and pen input go to the backend unchanged.

struct GLFWwindow;

namespace smatchet {
namespace standalone {

/// Install the key, cursor-position, mouse-button and cursor-enter callbacks on `window`. Call once,
/// right after ImGui_ImplGlfw_InitForOpenGL / InitForOther (which installed the backend's own), and
/// after smatchet::ui::touch::InstallTouchInputRouter on the current context.
void InstallStandaloneInputBridges(GLFWwindow* window);

} // namespace standalone
} // namespace smatchet

#endif // SMATCHET_STANDALONE_STANDALONETOUCHBRIDGE_H
