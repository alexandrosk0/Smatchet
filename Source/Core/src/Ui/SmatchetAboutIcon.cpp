#include "Ui/SmatchetAboutIcon.h"

#include "Logger.h"
#include "Persistence/SmatchetIcoDecode.h"
#include "Ui/AboutIconLatchPure.h"

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

// Failure-latch state. The policy — which failures are permanent and when to log — lives in
// Ui/AboutIconLatchPure.h so it is testable without ImGui or a live texture backend.
AboutIconLatch g_aboutIconLatch;

// Decoded bytes held ONLY while an upload has yet to succeed, so a renderer that never gains
// texture support costs one decode rather than one per frame on a draw path. Released as soon as
// the upload lands — the cache owns the texture from then on.
std::vector<unsigned char> g_aboutIconRgba;
int g_aboutIconWidth = 0;
int g_aboutIconHeight = 0;

} // namespace

bool TryGetAboutIconTexture(SmatchetLoadedIconTexture& out) {
    if (SmatchetImageTextureCache::TryGetCached(kAboutIconCacheKey, out)) {
        return true;
    }
    if (AboutIconAttemptsBlocked(g_aboutIconLatch)) {
        return false;
    }

#if SMATCHET_ICON_BYTES_AVAILABLE
    if (g_aboutIconRgba.empty()) {
        std::string error;
        if (!smatchet::icons::DecodeIcoToRgba(kSmatchetIconIcoBytes, sizeof(kSmatchetIconIcoBytes), g_aboutIconRgba,
                                              g_aboutIconWidth, g_aboutIconHeight, error)) {
            const AboutIconFailureAction action = OnAboutIconFailure(IconLoadStep::Decode, g_aboutIconLatch);
            g_aboutIconRgba.clear(); // DecodeIcoToRgba clears on entry, but do not rely on that here
            if (action.shouldLog) {
                LOG_WARN("About: app icon decode failed (%s); falling back to a glyph.", error.c_str());
            }
            return false;
        }
    }

    Result<SmatchetLoadedIconTexture> uploaded = SmatchetImageTextureCache::GetOrCreateFromRgba(
        kAboutIconCacheKey, g_aboutIconRgba, g_aboutIconWidth, g_aboutIconHeight);
    if (!uploaded) {
        // Not a latch: the usual cause is the renderer not yet advertising texture support, which a
        // later frame can satisfy. Warn once so a genuinely permanent failure is not silent either.
        const AboutIconFailureAction action = OnAboutIconFailure(IconLoadStep::Upload, g_aboutIconLatch);
        if (action.shouldLog) {
            LOG_WARN("About: app icon upload failed (%s); falling back to a glyph (will retry).",
                     uploaded.error().c_str());
        }
        return false;
    }
    // The cache owns the texture now; drop our copy of the pixels.
    std::vector<unsigned char>().swap(g_aboutIconRgba);
    out = uploaded.value();
    return true;
#else
    // No bytes were baked into this build — that never changes, so it latches.
    const AboutIconFailureAction action = OnAboutIconFailure(IconLoadStep::NoBytesBaked, g_aboutIconLatch);
    if (action.shouldLog) {
        LOG_WARN("About: no app icon was baked into this build; falling back to a glyph.");
    }
    return false;
#endif
}

} // namespace ui
} // namespace smatchet
