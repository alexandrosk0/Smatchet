// Small TU so diagnostics C-API symbols always link into SmatchetImGuiHost_DX12.lib
// (avoids "rebuilt main .cpp but lib not repackaged" confusion).

#include "SmatchetImGuiHost.h"
#include "SmatchetImGuiHostC.h"

#if defined(_WIN32)
#include "imgui.h"
#endif

#include <cstddef>

extern "C" {

// Bump this string whenever Source_Core changes in a way you want to verify made it into the
// packaged Unreal lib. It appears in:
//   LogSmatchetImGuiPlugin: Smatchet packaged native host tag: ...
// on plugin startup, so the editor log immediately reveals if Unreal is loading a stale lib.
#define SMATCHET_HOST_BUILD_TAG_REV "v2"

const char* SmatchetHost_GetBuildTag(void) {
#if defined(_WIN32)
    return "SmatchetImGuiHost " SMATCHET_HOST_BUILD_TAG_REV " InitInfo+Queue IMGUI_" IMGUI_VERSION " | built " __DATE__
           " " __TIME__;
#else
    return "SmatchetImGuiHost " SMATCHET_HOST_BUILD_TAG_REV;
#endif
}

const char* SmatchetHost_GetLastInitError(SmatchetImGuiHostHandle host) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h) {
        return "";
    }
    return h->PeekLastInitErrorUtf8();
}

void SmatchetHost_FormatCachedRendererSummary(SmatchetImGuiHostHandle host, char* buf, int bufSize) {
    auto* h = reinterpret_cast<SmatchetImGuiHost*>(host);
    if (!h || !buf || bufSize <= 0) {
        if (buf && bufSize > 0) {
            buf[0] = '\0';
        }
        return;
    }
    h->FormatCachedRendererDebugSummary(buf, static_cast<std::size_t>(bufSize));
}

} // extern "C"






