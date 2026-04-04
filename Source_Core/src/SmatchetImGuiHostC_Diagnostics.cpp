// Small TU so diagnostics C-API symbols always link into SmatchetImGuiHost_DX12.lib
// (avoids "rebuilt main .cpp but lib not repackaged" confusion).

#include "SmatchetImGuiHost.h"
#include "SmatchetImGuiHostC.h"

#if defined(_WIN32)
#include "imgui.h"
#endif

#include <cstddef>

extern "C" {

const char* SmatchetHost_GetBuildTag(void) {
#if defined(_WIN32)
    return "SmatchetImGuiHost InitInfo+Queue IMGUI_" IMGUI_VERSION;
#else
    return "SmatchetImGuiHost";
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
