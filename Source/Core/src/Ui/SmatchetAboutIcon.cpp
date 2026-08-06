#include "Ui/SmatchetAboutIcon.h"

#include "Logger.h"
#include "Persistence/SmatchetIcoDecode.h"

#include <string>
#include <vector>

// The generated header only exists once CMake has configured this tree. The __has_include guard
// mirrors AboutInfo.cpp, so an out-of-tree build of Source/Core still compiles (icon absent, caller
// falls back to a glyph). __has_include is formally C++17; the defined() wrapper keeps a bare C++14
// compiler happy, and both MSVC 2015u2+ and Clang expose it as an extension.
#if defined(__has_include)
#if __has_include(<SmatchetIconBytes.h>)
#include <SmatchetIconBytes.h>
#endif
#endif
#ifndef SMATCHET_ICON_BYTES_AVAILABLE
#define SMATCHET_ICON_BYTES_AVAILABLE 0
#endif

namespace smatchet {
namespace ui {

namespace {

const char* const kAboutIconCacheKey = "builtin:about-icon";

// Decode + upload are deterministic: if they fail once they fail every frame, and this runs from a
// draw path. Latch the failure so a missing icon costs one log line, not one per frame.
bool g_aboutIconFailed = false;

} // namespace

bool TryGetAboutIconTexture(SmatchetLoadedIconTexture& out) {
    if (SmatchetImageTextureCache::TryGetCached(kAboutIconCacheKey, out)) {
        return true;
    }
    if (g_aboutIconFailed) {
        return false;
    }

#if SMATCHET_ICON_BYTES_AVAILABLE
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    std::string error;
    if (!smatchet::icons::DecodeIcoToRgba(kSmatchetIconIcoBytes, sizeof(kSmatchetIconIcoBytes), rgba, width, height,
                                          error)) {
        g_aboutIconFailed = true;
        LOG_WARN("About: app icon decode failed (%s); falling back to a glyph.", error.c_str());
        return false;
    }

    Result<SmatchetLoadedIconTexture> uploaded =
        SmatchetImageTextureCache::GetOrCreateFromRgba(kAboutIconCacheKey, rgba, width, height);
    if (!uploaded) {
        g_aboutIconFailed = true;
        LOG_WARN("About: app icon upload failed (%s); falling back to a glyph.", uploaded.error().c_str());
        return false;
    }
    out = uploaded.value();
    return true;
#else
    g_aboutIconFailed = true;
    LOG_WARN("About: no app icon was baked into this build; falling back to a glyph.");
    return false;
#endif
}

} // namespace ui
} // namespace smatchet
