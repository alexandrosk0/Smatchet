#ifndef SMATCHET_STANDALONE_STANDALONEBOOT_DETAIL_H
#define SMATCHET_STANDALONE_STANDALONEBOOT_DETAIL_H

// Shared standalone-boot helpers, de-duplicated from main.cpp (the visible GUI
// boot) and StandaloneAppBootstrap.cpp (the hidden-window / ephemeral boot). Both
// boot paths historically carried byte-for-byte copies of these small path /
// callback / pixel helpers — the dup_audit `duplication` WARN flagged the pairs.
// Behaviour is preserved exactly; the two boot loops still own their distinct
// frame/window lifetimes, only these leaf helpers are now single-sourced.
//
// GLFW / ImGui types are forward-declared so this header stays light — the .cpp
// pulls in <GLFW/glfw3.h> + imgui_impl_glfw.h. Standalone-only (never included by
// Source/Core), so the GLFW forward decl does not threaten the DX12 dual-target.

#include <string>
#include <vector>

struct GLFWwindow;

namespace smatchet {
namespace standalone {
namespace boot_detail {

/// Normalize a directory path: backslashes -> '/', ensure a trailing '/'. Empty
/// in -> empty out (callers treat "" as "unset").
std::string NormalizeDirectory(std::string path);

/// Best-effort recursive create of `path` (ghc::filesystem::create_directories).
/// Silent on failure (error_code swallowed) — callers fall back gracefully.
void EnsureDirectoryExists(const std::string& path);

/// Resolve the writable standalone user-data dir from the executable directory:
/// the portable marker keeps data beside the exe, otherwise the OS shared dir
/// (falling back to the exe dir when the OS dir can't be resolved).
std::string ResolveStandaloneUserDataDir(const std::string& exeDir);

/// GLFW key callback that bridges Keypad-Enter to the standard Enter key before
/// forwarding to ImGui's GLFW backend (so numpad Enter confirms dialogs).
void KeypadEnterBridgeCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

/// Pack a top-left-origin RGBA8 buffer into tightly-packed RGB8, dropping alpha.
/// `flipVertical` reads rows bottom-up (GL front-buffer readback is bottom-left
/// origin; DX12 readback is already top-left, so it passes false).
void PackRgbDropAlpha(const unsigned char* rgbaSrc, int w, int h, bool flipVertical, std::vector<unsigned char>& rgb);

} // namespace boot_detail
} // namespace standalone
} // namespace smatchet

#endif // SMATCHET_STANDALONE_STANDALONEBOOT_DETAIL_H
