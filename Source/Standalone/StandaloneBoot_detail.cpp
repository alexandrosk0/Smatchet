#include "StandaloneBoot_detail.h"

#include "Commands/Scenarios/ScenarioCaptureSizing.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetUiSession.h"

// stb_image_write single-TU implementation. Lives here, not in the two boot TUs:
// both used to carry their own STATIC copy purely to write the screenshot PNG, and
// WriteCapturePng below is now the only caller in the standalone target.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#if defined(_MSC_VER)
// Third-party stb header — silence ALL MSVC warnings for the impl block (4996
// sprintf, 4505 unused-static under reduced-feature/Debug configs, …) so
// first-party /WX stays strict without whack-a-mole per warning code.
#pragma warning(push, 0)
#endif
#include <stb/stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"

#include <GLFW/glfw3.h>

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <cstddef>
#include <system_error>

// g_ui is defined in SmatchetUI.cpp; the header-side extern is gated on
// SMATCHET_WITH_LUA_AUTOMATION, so declare it unconditionally here — the same shim the
// scenario TUs use.
extern UiDrawSession g_ui;

namespace smatchet {
namespace standalone {
namespace boot_detail {

std::string NormalizeDirectory(std::string path) {
    if (path.empty()) {
        return path;
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.back() != '/') {
        path.push_back('/');
    }
    return path;
}

void EnsureDirectoryExists(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    ghc::filesystem::create_directories(ghc::filesystem::path(path), ec);
}

std::string ResolveStandaloneUserDataDir(const std::string& exeDir) {
    const ConfigManager::StoragePreference pref =
        ConfigManager::GetStoragePreference(exeDir, ConfigManager::StoragePreference::Shared);
    if (pref == ConfigManager::StoragePreference::Portable) {
        return exeDir;
    }
    std::string osDir = ConfigManager::GetPlatformSharedUserDataDirectory();
    if (!osDir.empty()) {
        return osDir;
    }
    return exeDir;
}

void KeypadEnterBridgeCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key == GLFW_KEY_KP_ENTER) {
        ImGui_ImplGlfw_KeyCallback(window, GLFW_KEY_ENTER, scancode, action, mods);
    }
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
}

void PackRgbDropAlpha(const unsigned char* rgbaSrc, int w, int h, bool flipVertical, std::vector<unsigned char>& rgb) {
    const std::size_t rowBytesRgb = static_cast<std::size_t>(w) * 3u;
    rgb.assign(static_cast<std::size_t>(h) * rowBytesRgb, 0);
    for (int y = 0; y < h; ++y) {
        const int srcRow = flipVertical ? (h - 1 - y) : y;
        const unsigned char* src = &rgbaSrc[static_cast<std::size_t>(srcRow) * static_cast<std::size_t>(w) * 4u];
        unsigned char* dst = &rgb[static_cast<std::size_t>(y) * rowBytesRgb];
        for (int x = 0; x < w; ++x) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst += 3;
            src += 4;
        }
    }
}

void WriteCapturePng(const std::string& path, const std::vector<unsigned char>& rgb, int w, int h) {
    stbi_write_png_compression_level = 8;
    const int rc = stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
    if (rc == 0) {
        LOG_ERROR("debug.window.screenshot: stbi_write_png failed for %s", path.c_str());
        return;
    }
    g_ui.lastCaptureWidth = w;
    g_ui.lastCaptureHeight = h;
    LOG_INFO("debug.window.screenshot: saved %s (%dx%d, PNG)", path.c_str(), w, h);
    ReportCaptureSizeContract(w, h);
}

void ReportCaptureSizeContract(int capturedWidth, int capturedHeight) {
    if (g_ui.expectedCaptureWidth <= 0 || g_ui.expectedCaptureHeight <= 0) {
        return; // no scenario contract armed — nothing to hold this capture to
    }
    smatchet::cmd::ScenarioCaptureSize expected;
    expected.Width = g_ui.expectedCaptureWidth;
    expected.Height = g_ui.expectedCaptureHeight;
    // One-shot: disarm before returning so a later unrelated capture (a bug-report shot)
    // is never judged against this scenario's contract.
    g_ui.expectedCaptureWidth = -1;
    g_ui.expectedCaptureHeight = -1;
    const smatchet::cmd::ScenarioCaptureSizeVerdict verdict =
        smatchet::cmd::VerifyScenarioCaptureSize(expected, capturedWidth, capturedHeight);
    if (!verdict.Ok) {
        LOG_ERROR("debug.window.screenshot: %s", verdict.Message.c_str());
    }
}

} // namespace boot_detail
} // namespace standalone
} // namespace smatchet
