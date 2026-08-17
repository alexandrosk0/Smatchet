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

/// Enforce the scenario capture-size contract on a just-written capture, then disarm it. A
/// scenario only *requests* its capture size and cannot check the outcome itself (its OnFinish
/// merely asks for the screenshot, which lands frames later), so this capture path — the first
/// place both the request and the achieved framebuffer are known — is where a window-system
/// clamp is caught. Logged at ERROR (#2092). No-op when no contract is armed.
void ReportCaptureSizeContract(int capturedWidth, int capturedHeight);

/// Encode `rgb` to `path` as a PNG at compression level 8, then record + verify the capture
/// size. Shared tail of both boot paths' screenshot handlers, which otherwise carried
/// byte-identical copies of the write/log/verify block.
void WriteCapturePng(const std::string& path, const std::vector<unsigned char>& rgb, int w, int h);

} // namespace boot_detail
} // namespace standalone
} // namespace smatchet

#endif // SMATCHET_STANDALONE_STANDALONEBOOT_DETAIL_H
