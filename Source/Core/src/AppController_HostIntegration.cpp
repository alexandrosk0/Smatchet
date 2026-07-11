// AppController_HostIntegration.cpp — host-integration cluster extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/appcontroller-clusters-followup.md). Method DECLARATIONS stay in
// AppController.h; only the definitions moved, so linkage and behavior are identical.
// The cluster is the surface an embedding shell registers (the host-callback setters)
// plus the actions that invoke it: closing the embedded UI, requesting app quit,
// opening URLs (scheme allowlist plus the per-platform system fallback when no host
// handler is set), the automation sink registration wired up at early init, and the
// file-open-dialog request. The no-shell launch helper moves along — its only caller
// is the URL-open fallback. Includes are curated from what the moved bodies actually
// use. No winsock preamble — this TU pulls no cpr/curl header; the platform block
// below exists solely for the system URL-open fallback.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining the AppController host-integration methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on
#include "AppControllerImpl.h"
// The pImpl inline constructor instantiates the destructor of every unguarded owned member in
// each including TU, so the Lua automation host must be complete here — and the automation
// sink delegators below also call through it. Mirrors the sibling companion TUs that include
// the impl header.
#include "LuaAutomationHost.h"

#include "Logger.h"
#include "StringUtil.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

namespace {

#if defined(__APPLE__) || defined(__linux__)

// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=deliberate file-local twin of the no-shell launcher in AttachmentAppUpdateService.cpp — each TU keeps its own copy so neither grows a header dependency on the other for a ten-line fork-exec wrapper; relocating the AppController copy out of AppController.cpp re-hashed the grandfathered pair, not a new copy-paste; owner=orchestrator; revisit=when a shared process-spawn utility header exists)
// clang-format on
bool LaunchCommandNoShell(const char* exe, const std::string& arg) {

    if (!exe || arg.empty()) {

        return false;
    }

    const pid_t child = fork();

    if (child < 0) {

        return false;
    }

    if (child == 0) {

        execlp(exe, exe, arg.c_str(), static_cast<char*>(nullptr));

        _exit(127);
    }

    return true;
}

#endif

} // namespace

void AppController::SetOpenUrlHandler(std::function<void(const std::string&)> handler) {

    hostCallbacks_.OpenUrl = std::move(handler);
}

void AppController::SetCloseEmbeddedUiHandler(std::function<void()> handler) {

    hostCallbacks_.CloseEmbeddedUi = std::move(handler);
}

void AppController::CloseEmbeddedUi() {

    if (hostCallbacks_.CloseEmbeddedUi) {

        hostCallbacks_.CloseEmbeddedUi();
    }
}

void AppController::SetRequestAppQuitHandler(std::function<void()> handler) {
    hostCallbacks_.RequestAppQuit = std::move(handler);
}

void AppController::RequestAppQuit() const {

    if (hostCallbacks_.RequestAppQuit) {

        hostCallbacks_.RequestAppQuit();
    }
}

void AppController::SetRuntimePluginHost(PluginHost* host) { runtimePluginHost_ = host; }

void AppController::OpenUrl(const std::string& url) const {

    if (url.empty()) {

        return;
    }

    // Scheme allowlist: avoid handing `javascript:`, `file:`, `vbscript:`, etc.

    // Only http(s) and mailto pass through; anything else is rejected.

    {

        std::string schemePrefix;

        const size_t colonPos = url.find(':');

        if (colonPos == std::string::npos) {

            LOG_WARN("AppController::OpenUrl rejected: missing scheme in url=%s", TruncateForLog(url, 200).c_str());

            return;
        }

        schemePrefix.reserve(colonPos);

        for (size_t i = 0; i < colonPos; ++i) {

            schemePrefix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(url[i]))));
        }

        const bool ok = (schemePrefix == "http") || (schemePrefix == "https") || (schemePrefix == "mailto");

        if (!ok) {

            LOG_WARN("AppController::OpenUrl rejected: scheme '%s' not allowlisted for url=%s", schemePrefix.c_str(),

                     TruncateForLog(url, 200).c_str());

            return;
        }
    }

    if (hostCallbacks_.OpenUrl) {

        hostCallbacks_.OpenUrl(url);

        return;
    }

#if defined(_WIN32)

    const HINSTANCE openResult = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    if (reinterpret_cast<intptr_t>(openResult) <= 32) {

        LOG_ERROR("AppController::OpenUrl failed url=%s err=%ld", TruncateForLog(url, 300).c_str(), GetLastError());
    }

#elif defined(__APPLE__)

    if (!LaunchCommandNoShell("open", url)) {

        LOG_ERROR("AppController::OpenUrl failed to launch url=%s", TruncateForLog(url, 300).c_str());
    }

#else

    if (!LaunchCommandNoShell("xdg-open", url)) {

        LOG_ERROR("AppController::OpenUrl failed to launch url=%s", TruncateForLog(url, 300).c_str());
    }

#endif
}

// AddAutomationLogSink / ClearAutomationLogSinks moved to LuaAutomationHost in Phase 1A of
// the item 14 extraction. Thin delegators below.

void AppController::AddAutomationLogSink(std::function<void(const std::string&)> sink) {
    if (impl_->luaHost_) {
        impl_->luaHost_->AddAutomationLogSink(std::move(sink));
    } else {
        // OnEarlyInit fires before Initialize constructs the Lua host, so the sink is
        // buffered here and drained into the host immediately after construction.
        if (sink) {
            pendingLogSinks_.push_back(std::move(sink));
        }
    }
}

void AppController::ClearAutomationLogSinks() {
    if (impl_->luaHost_) {
        impl_->luaHost_->ClearAutomationLogSinks();
    }
}

void AppController::AddAutomationErrorSink(std::function<void(const std::string&)> sink) {
    errorSinks_.push_back(std::move(sink));
}

bool AppController::ConsumeScriptingWindowRequest() { return scriptingWindowOpenRequested_.exchange(false); }

void AppController::SetAttachmentViewerHandler(AttachmentViewerHandler handler) {

    hostCallbacks_.AttachmentViewer = std::move(handler);
}

void AppController::SetAttachmentPreviewHandler(AttachmentPreviewHandler handler) {

    hostCallbacks_.AttachmentPreview = std::move(handler);
}

void AppController::SetAttachmentCollectionHandler(AttachmentCollectionHandler handler) {

    hostCallbacks_.AttachmentCollection = std::move(handler);
}

void AppController::SetOpenFilePathsHandler(OpenFilePathsHandler handler) {

    hostCallbacks_.OpenFilePaths = std::move(handler);
}

void AppController::RequestOpenFilePaths(bool allowMultiple, const std::string& initialDirectoryUtf8,

                                         std::function<void(std::vector<std::string>)> onComplete) const {

    if (!onComplete) {

        return;
    }

    if (hostCallbacks_.OpenFilePaths) {

        hostCallbacks_.OpenFilePaths(allowMultiple, initialDirectoryUtf8, std::move(onComplete));

        return;
    }

    onComplete({});
}
