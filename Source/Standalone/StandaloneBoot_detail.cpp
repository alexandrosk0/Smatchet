#include "StandaloneBoot_detail.h"

#include "ConfigManager.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"

#include <GLFW/glfw3.h>

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <cstddef>
#include <system_error>

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

} // namespace boot_detail
} // namespace standalone
} // namespace smatchet
