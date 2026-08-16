// winsock2.h must be the FIRST Win32-related include in this TU: ghc/filesystem and many
// other headers transitively pull in <windows.h>, which auto-includes the legacy <winsock.h>
// unless WIN32_LEAN_AND_MEAN is defined. Once winsock1 has been included, a later <winsock2.h>
// fires `#warning Please include winsock2.h before windows.h` from MinGW's headers. Putting
// the winsock2 block at the very top (with WIN32_LEAN_AND_MEAN) avoids both the warning and
// the symbol-conflict it foreshadows.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <ghc/filesystem.hpp>
#include "Logger.h"

#if defined(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME)
static bool g_MainWindowShownAfterFirstFrame = false;
#endif

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// ImGui Core and Backends
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "SmatchetImGuiFonts.h"

// stb_image_write — single-TU implementation lives here so SmatchetStandalone
// can encode PNGs for debug.window.screenshot output (replaces the prior raw
// PPM dump that bloated tests/golden/* by ~40×). Source/Core/ThirdParty/ is on
// this target's include path via smatchet_configure_opengl_core_impl_target.
// stb_image_write emits TGA / BMP / JPG / HDR helpers we never call —
// suppress -Wunused-function for the impl block so the warnings don't drown
// out real issues in main.cpp.
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
#if defined(SMATCHET_BUILD_UI_TESTS)
// Pulls in the engine API (PostSwap) and the SmatchetActiveUiTestEngine accessor
// without dragging nlohmann::json into main.cpp (UiTestScenario.h forward-declares
// the engine struct and keeps json out of the header).
#include "Commands/Scenarios/UiTestScenario.h"
#include "imgui_te_engine.h"
#endif
#include <GLFW/glfw3.h> // Will drag in system OpenGL headers

#if defined(_WIN32)
// Needed to call glfwGetWin32Window().
#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

// Smatchet Core Headers (Safe C++14 includes)
#include "ConfigManager.h"
#include "VsyncControl.h"
#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "CliCommandRunner.h"
#include "StandaloneAppBootstrap.h"
#include "StandaloneBoot_detail.h"
#include "Dx12Bootstrap.h" // StandaloneRenderer + optional DX12 backend (no-op shell off-Windows)
// AppController holds unique_ptr<> members of these service types; main.cpp's
// stack instance triggers destructor / noexcept evaluation that requires the
// complete types, so include them here (CODE_REVIEW items 11/12/13/14).
#include "GridContextDepsAdapter.h"
#include "ITrackerBackendFactory.h"
#include "LuaAutomationHost.h"
#include "OfflineQueueService.h"
#include "TicketSyncService.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "PluginHost.h"
#include "SmatchetUI.h"
#include "Ui/SmatchetImGuiTextureGuardRuntime.h"
#include "Ui/SmatchetTooltipWheelRouter.h"

#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif

// UiDrawSession is defined in SmatchetUI.cpp (translation-unit global g_ui).
// main.cpp reads requestFullScreenToggle and cfg.FullScreen after each frame.
#include "SmatchetUiSession.h"
#include "ScreenshotCensor.h"      // log-a-bug-github — mosaic censor for the screenshot capture
#include "Diagnostics/CrashSink.h" // log-a-bug-github phase 2 — crash marker + minidump survival
#include "SmatchetCrashHandler.h"  // log-a-bug-github phase 2 — install OS crash handlers
extern UiDrawSession g_ui;

// GLFW Error Callback
static void glfw_error_callback(int error, const char* description) {
    ::fprintf(stderr, "GLFW Error %d: %s\n", error, description); // pre-logger-init — LOG_* unavailable
}

// Keypad-Enter -> Enter bridge lives in StandaloneBoot_detail (shared with the
// hidden-window bootstrap). Aliased so the glfwSetKeyCallback site stays terse.
using smatchet::standalone::boot_detail::KeypadEnterBridgeCallback;

#if defined(_WIN32)
static void SmatchetApplyWindowIcon(GLFWwindow* window) {
    if (!window)
        return;

    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd)
        return;

    // Load the icon embedded in the executable resources (see smatchet.rc).
    // Use the canonical IDI_APPLICATION numeric id (32512).
    const WORD kIconResourceId = 32512; // IDI_APPLICATION
    HMODULE hModule = GetModuleHandleA(NULL);
    HICON hIcon = LoadIconA(hModule, MAKEINTRESOURCE(kIconResourceId));
    if (!hIcon) {
        // Fallback for cases where LoadIcon fails for the resource.
        hIcon = (HICON)LoadImageA(hModule, MAKEINTRESOURCE(kIconResourceId), IMAGE_ICON, 0, 0, LR_SHARED);
    }
    if (!hIcon) {
        // If this prints, the `.rc` resource may not actually be embedded/recognized.
        // This is only for debugging the icon issue.
        HRSRC groupRes = FindResourceA(hModule, MAKEINTRESOURCE(kIconResourceId), RT_GROUP_ICON);
        if (!groupRes) {
            ::fprintf(stderr, "Icon not found in resources (RT_GROUP_ICON) id=%u\n",
                      (unsigned)kIconResourceId); // pre-logger-init — LOG_* unavailable
        } else {
            ::fprintf(stderr, "Icon resource exists but LoadIcon/LoadImage failed id=%u\n",
                      (unsigned)kIconResourceId); // pre-logger-init — LOG_* unavailable
        }
        return;
    }

    // Set both big and small icons so it shows up consistently in title bar/taskbar.
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

    // Also set class icons (sometimes required for taskbar / alt-tab consistency).
    SetClassLongPtrW(hwnd, GCLP_HICON, (LONG_PTR)hIcon);
    SetClassLongPtrW(hwnd, GCLP_HICONSM, (LONG_PTR)hIcon);
}
#endif

static void SmatchetDrawFrame(SmatchetUI& mainWindow, AppController& smatchetApp, PluginHost& pluginHost) {
    try {
        mainWindow.Draw(smatchetApp);
        pluginHost.OnDraw(smatchetApp);
    } catch (const std::exception&) {
        throw;
    } catch (...) { // catch-all-ok: rethrow only — propagates to the SEH wrapper, swallows nothing
        throw;
    }
}

// MSVC: __try cannot appear in a function that needs C++ object unwinding (e.g. main with PluginHost on stack).
static void SmatchetDrawFrameWithSeh(SmatchetUI& mainWindow, AppController& smatchetApp, PluginHost& pluginHost) {
#if defined(_WIN32) && defined(_MSC_VER)
    __try {
        SmatchetDrawFrame(mainWindow, smatchetApp, pluginHost);
    } __except (smatchet::SmatchetCrashSehFilter(GetExceptionInformation())) {
        // Marker + minidump already written by the filter; next launch reports it.
        std::exit(1);
    }
#else
    SmatchetDrawFrame(mainWindow, smatchetApp, pluginHost);
#endif
}

static void SmatchetLogOpenGLInfo() {
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* shading = glGetString(GL_SHADING_LANGUAGE_VERSION);
    ::fprintf(stderr, "OpenGL vendor: %s\n",
              vendor ? reinterpret_cast<const char*>(vendor) : "(null)"); // pre-logger-init — LOG_* unavailable
    ::fprintf(stderr, "OpenGL renderer: %s\n",
              renderer ? reinterpret_cast<const char*>(renderer) : "(null)"); // pre-logger-init — LOG_* unavailable
    ::fprintf(stderr, "OpenGL version: %s\n",
              version ? reinterpret_cast<const char*>(version) : "(null)"); // pre-logger-init — LOG_* unavailable
    ::fprintf(stderr, "OpenGL GLSL: %s\n",
              shading ? reinterpret_cast<const char*>(shading) : "(null)"); // pre-logger-init — LOG_* unavailable
}

// NormalizeDirectory / EnsureDirectoryExists live in StandaloneBoot_detail (shared
// with the hidden-window bootstrap). Aliased so the call sites below stay terse.
using smatchet::standalone::boot_detail::EnsureDirectoryExists;
using smatchet::standalone::boot_detail::NormalizeDirectory;

namespace smatchet {
namespace standalone {
namespace {

// File-local boot state threaded from BootApplication() into RunFrameLoop().
// Carries the GLFW window, the per-frame app/plugin handles (owned by bootCtx),
// the persisted window-state cfg peeked from config, and the resolved log path.
// Mirrors BootstrapContext's role but for the *visible* standalone boot, which
// owns its ImGui/GLFW lifetime directly in main() (vs the bootstrap helpers'
// hidden-window path). main() fills this via BootApplication, hands it to
// RunFrameLoop, then tears down ImGui/GLFW after the loop returns.
struct MainBootState {
    BootstrapContext bootCtx;
    TrackerConfig windowStateCfg;
    int initialWindowW = 0;
    int initialWindowH = 0;
    bool restoreMaximized = false;
    std::string logPath; // recorded for the crash reporter's next-launch log-tail
    // Renderer resolved once in BootApplication and carried through the frame
    // loop. `dx12` owns the D3D12 swapchain when renderer == Dx12; it is a
    // no-op shell (never constructed) on the OpenGL path and off-Windows.
    smatchet::standalone::StandaloneRenderer renderer = smatchet::standalone::StandaloneRenderer::OpenGL;
    std::unique_ptr<smatchet::standalone::Dx12Bootstrap> dx12;
};

// Resolve the SMATCHET_USER_DATA override + wire the default file log sink.
// Runs FIRST in main() — before the cmd/ephemeral short-circuits — so those
// agent-facing paths inherit the same user-data dir + log file as the GUI boot
// (order-preserving extraction of main()'s original prologue). Records the
// resolved path into `boot.logPath` for the crash reporter's next-launch tail.
static void WireUserDataAndLogSink(MainBootState& boot) {
    // SMATCHET_USER_DATA — override the writable user data directory.
    // Applied here, before ConfigManager::Load(), so the config file, SQLite DB,
    // views, recents, and instance.json all resolve under the overridden path.
    // Stable API: see docs/plans/active/applied/command-system-plan.md §"Environment contract".
    {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
        const char* envUserData = std::getenv("SMATCHET_USER_DATA");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (envUserData && envUserData[0] != '\0') {
            const std::string userDataDir = NormalizeDirectory(std::string(envUserData));
            EnsureDirectoryExists(userDataDir);
            ConfigManager::SetUserDataDirectory(userDataDir);
        }
    }

    // Default file-sink wire — addresses the debug-detective backlog entry at
    // docs/self-improvement/categories/tooling.md "Smatchet Logger has no
    // console output + no default file sink". Until this default wire shipped,
    // every `LOG_INFO` / `LOG_DEBUG` from Smatchet itself was captured only in
    // the in-memory deque + (unwired in standalone) async file sink; running
    // `Smatchet.exe 2>&1 > out.log` captured only OpenGL banner + whisper.cpp
    // stderr. Every debug-detective investigation paid the cost of wiring a
    // temp file sink mid-investigation; this default removes that tax.
    // Resolution order: SMATCHET_DEBUG_LOG env wins (operator override); else
    // %LOCALAPPDATA%\Smatchet\Smatchet-<pid>.log on Windows / $TMPDIR fallback.
    // The per-PID suffix means concurrent Smatchet processes (e.g. spawn-mode
    // ephemeral + the user's manual instance) get distinct log files instead
    // of interleaving + corrupting a shared sink.
    std::string& logPath = boot.logPath; // crash reporter records it for next-launch log-tail
    {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
        const char* envLog = std::getenv("SMATCHET_DEBUG_LOG");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (envLog && envLog[0] != '\0') {
            logPath = envLog;
        } else {
#if defined(_WIN32)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
            const char* localAppData = std::getenv("LOCALAPPDATA");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
            if (localAppData && localAppData[0] != '\0') {
                const std::string dir = std::string(localAppData) + "\\Smatchet";
                EnsureDirectoryExists(dir);
                char pidBuf[32];
                std::snprintf(pidBuf, sizeof(pidBuf), "Smatchet-%lu.log",
                              static_cast<unsigned long>(GetCurrentProcessId()));
                logPath = dir + "\\" + pidBuf;
            }
#else
            // Non-Windows: $TMPDIR + timestamp suffix. Avoids the unistd.h
            // / getpid portability dance — Smatchet's primary target is
            // Windows so the POSIX fallback is best-effort, and a timestamp
            // is sufficient to disambiguate concurrent processes.
            const char* tmp = std::getenv("TMPDIR");
            const std::string dir = (tmp && tmp[0] != '\0') ? std::string(tmp) : std::string("/tmp");
            char tsBuf[64];
            std::snprintf(tsBuf, sizeof(tsBuf), "/Smatchet-%lld.log", static_cast<long long>(std::time(nullptr)));
            logPath = dir + tsBuf;
#endif
        }
        if (!logPath.empty()) {
            Logger::Instance().SetFileSinkPath(logPath);
            // Use LOG_INFO so the launch path itself records where it landed;
            // future debug sessions read this line first to know which file
            // to tail. Logger::Instance() is process-static; safe pre-Load.
            LOG_INFO("Logger file sink: %s", logPath.c_str());
        }
    }
}

// Resolve the runtime-asset dir (exe dir) + the writable user-data dir from the
// running executable's path. Runs BEFORE window creation so the saved window
// pos/size/maximized state in config can drive the initial window hints.
static void ResolveExeAndUserDataDirs() {
    // Shared with the hidden-window bootstrap (StandaloneBoot_detail) — was a
    // file-local lambda, identical body.
    const auto& resolveStandaloneUserDataDir = boot_detail::ResolveStandaloneUserDataDir;

#if defined(_WIN32)
    {
        char buf[MAX_PATH];
        if (GetModuleFileNameA(NULL, buf, MAX_PATH)) {
            std::string exePath(buf);
            std::string::size_type last = exePath.find_last_of("\\/");
            if (last != std::string::npos) {
                std::string exeDir = NormalizeDirectory(exePath.substr(0, last + 1));
                ConfigManager::SetRuntimeAssetDirectory(exeDir);
                std::string userDataDir = resolveStandaloneUserDataDir(exeDir);
                EnsureDirectoryExists(userDataDir);
                ConfigManager::SetUserDataDirectory(userDataDir);
            }
        }
    }
#elif defined(__linux__)
    {
        char buf[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string exePath(buf);
            const std::string::size_type last = exePath.find_last_of('/');
            if (last != std::string::npos) {
                const std::string exeDir = NormalizeDirectory(exePath.substr(0, last + 1));
                ConfigManager::SetRuntimeAssetDirectory(exeDir);
                std::string userDataDir = resolveStandaloneUserDataDir(exeDir);
                EnsureDirectoryExists(userDataDir);
                ConfigManager::SetUserDataDirectory(userDataDir);
            }
        }
    }
#elif defined(__APPLE__)
    {
        std::vector<char> buf(PATH_MAX);
        uint32_t sz = static_cast<uint32_t>(buf.size());
        if (_NSGetExecutablePath(buf.data(), &sz) == 0) {
            std::string exePath(buf.data());
            const std::string::size_type last = exePath.find_last_of('/');
            if (last != std::string::npos) {
                const std::string exeDir = NormalizeDirectory(exePath.substr(0, last + 1));
                ConfigManager::SetRuntimeAssetDirectory(exeDir);
                std::string userDataDir = resolveStandaloneUserDataDir(exeDir);
                EnsureDirectoryExists(userDataDir);
                ConfigManager::SetUserDataDirectory(userDataDir);
            }
        }
    }
#endif
}

// GLFW init + crash-handler install + window creation. Resolves exe/user-data
// dirs, installs crash handlers, peeks saved window state into `boot`, applies
// GL/GLSL + visibility/maximize hints, creates the window, restores a saved
// on-screen position, applies the icon, and makes the context current with
// vsync. Returns the window on success, or null on a fatal init failure (the
// caller maps null to exit code 1). `outGlslVersion` receives the chosen GLSL
// string for the later renderer-backend init.
static GLFWwindow* BootInitGlfwAndWindow(MainBootState& boot, const char*& outGlslVersion) {
    const std::string& logPath = boot.logPath; // crash reporter records it for next-launch log-tail

    // 1. Setup OS Window (GLFW)
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return NULL;
    }

    ResolveExeAndUserDataDirs();

    // log-a-bug-github phase 2 — install crash handlers now that the user-data dir is
    // resolved (CrashSinkInit writes its marker/dump there). Runs before any heavy work
    // so startup crashes are still captured. CrashSinkInit builds the static path buffers
    // the async handlers use; InstallCrashHandlers wires SEH/terminate/signals.
    smatchet::diagnostics::CrashSinkInit(ConfigManager::GetUserDataDirectory(), logPath);
    smatchet::InstallCrashHandlers();
    // debug.dump_self's writer. Installed here, beside the crash handlers and after
    // CrashSinkInit has created <userData>/crashes, and BEFORE any command-serving
    // thread exists — the SelfDump seam relies on that ordering rather than a lock.
    smatchet::InstallSelfDumpProvider();
    smatchet::diagnostics::CrashSinkBreadcrumb("startup");

    // Peek window state from saved config so we can hint MAXIMIZED + initial size.
    // Stored into `boot` so RunFrameLoop can seed the windowed-bounds persistence.
    boot.windowStateCfg = ConfigManager::Load();
    const TrackerConfig& windowStateCfg = boot.windowStateCfg;
    boot.initialWindowW = std::max(320, windowStateCfg.WindowWidth);
    boot.initialWindowH = std::max(240, windowStateCfg.WindowHeight);
    boot.restoreMaximized = windowStateCfg.WindowMaximized;
    const int initialWindowW = boot.initialWindowW;
    const int initialWindowH = boot.initialWindowH;
    const bool restoreMaximized = boot.restoreMaximized;
    const bool havePosHint = (windowStateCfg.WindowX != -1 && windowStateCfg.WindowY != -1);

    // Renderer-specific window hints. The DX12 path creates the GLFW window with
    // no client API (we drive a D3D12 swapchain on the HWND directly, so GLFW
    // must not make a GL context); the GL path requests the 3.0/3.2 context as
    // before. boot.renderer was resolved in BootApplication before this call.
    const bool useDx12 = (boot.renderer == smatchet::standalone::StandaloneRenderer::Dx12);
    if (useDx12) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        outGlslVersion = NULL; // no GLSL on the DX12 path
    } else {
#if defined(__APPLE__)
        outGlslVersion = "#version 150";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
        outGlslVersion = "#version 130";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    }

#if defined(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#endif
    if (restoreMaximized) {
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    }

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(initialWindowW, initialWindowH, "Smatchet - Standalone", NULL, NULL);
    if (window == NULL) {
        // Pillar 3 (never crash silently): a NULL window means the driver/context
        // request failed. Surface it — logged for the next-launch tail and an OS
        // dialog so a windowless launch isn't a silent exit.
        LOG_ERROR("glfwCreateWindow failed (renderer=%s, %dx%d) — cannot open the standalone window.",
                  useDx12 ? "dx12" : "gl", initialWindowW, initialWindowH);
#if defined(_WIN32)
        MessageBoxA(NULL,
                    "Smatchet could not create its application window.\n\n"
                    "Your graphics driver may be missing or out of date.",
                    "Smatchet - Window creation failed", MB_OK | MB_ICONERROR);
#endif
        return NULL;
    }

    // Restore saved position if it still overlaps a connected monitor.
    // Skip when maximized — glfwSetWindowPos would un-maximize the window.
    if (havePosHint && !restoreMaximized) {
        auto rectOverlapsAnyMonitor = [](int x, int y, int w, int h) -> bool {
            int count = 0;
            GLFWmonitor** mons = glfwGetMonitors(&count);
            for (int i = 0; i < count; ++i) {
                int mx, my, mw, mh;
                glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
                const int ix = std::max(x, mx);
                const int iy = std::max(y, my);
                const int ax = std::min(x + w, mx + mw);
                const int ay = std::min(y + h, my + mh);
                if (ax - ix >= 100 && ay - iy >= 100)
                    return true;
            }
            return false;
        };
        if (rectOverlapsAnyMonitor(windowStateCfg.WindowX, windowStateCfg.WindowY, initialWindowW, initialWindowH)) {
            glfwSetWindowPos(window, windowStateCfg.WindowX, windowStateCfg.WindowY);
        }
    }

#if defined(_WIN32)
    SmatchetApplyWindowIcon(window);
#endif

    // GL-only context binding. The DX12 path has no GLFW GL context (GLFW_NO_API)
    // — calling these would assert; vsync there is per-Present(syncInterval).
    if (!useDx12) {
        glfwMakeContextCurrent(window);

        // Initial vsync-on; BootApplication re-applies the resolved value (env >
        // CLI flag > persisted config) right after ConfigManager::Load(cli).
        glfwSwapInterval(1);
    }

    return window;
}

// ImGui context + ini-schema migration + style/font + platform/renderer init.
// Runs after the GL context is current. Returns false if the OpenGL device
// objects fail to create (caller maps that to exit code 1).
static bool BootSetupImGui(GLFWwindow* window, const char* glsl_version, MainBootState& boot) {
    // 2. Setup Dear ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string s_imguiIniPath = ConfigManager::GetImGuiSettingsPath();
    // Schema migration must run before ImGui auto-loads the ini. If we wait for
    // SmatchetUI::Draw (post-first-frame) to migrate, windows are already created
    // at old positions and runtime LoadIniSettingsFromDisk does not re-parent them.
    {
        TrackerConfig bootCfg = ConfigManager::Load();
        if (bootCfg.LayoutSchemaVersion < ConfigManager::kCurrentLayoutSchemaVersion) {
            ConfigManager::WriteDefaultImGuiSettingsFile();
            bootCfg.LayoutSchemaVersion = ConfigManager::kCurrentLayoutSchemaVersion;
            ConfigManager::Save(bootCfg);
        } else {
            ConfigManager::EnsureDefaultImGuiSettingsFile();
        }
    }
    io.IniFilename = s_imguiIniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking

    // Setup ImGui Style.
    // StyleColorsDark seeds ImGui's default-dark substrate (#FFFFFF text,
    // #4296FA blue-on-translucent button hover, FramePadding [4,3], rounding
    // 0). SmatchetTheme::ApplyStyle(SmatchetDark) is what actually paints the
    // Smatchet look — #F2F2F2 text on #1F1F24 panels, the #383842 button base
    // with #598CF2 accent, FramePadding [6,4], the 4-6 rounding family.
    ImGui::StyleColorsDark();
    SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io);

    // Setup Platform/Renderer bindings
    if (boot.renderer == smatchet::standalone::StandaloneRenderer::Dx12) {
        // DX12: the GLFW platform backend runs in no-GL ("Other") mode; the
        // renderer backend is the D3D12 swapchain owned by boot.dx12.
        ImGui_ImplGlfw_InitForOther(window, true);
        glfwSetKeyCallback(window, KeypadEnterBridgeCallback);

        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);
#if defined(_WIN32)
        void* hwnd = glfwGetWin32Window(window);
#else
        void* hwnd = nullptr;
#endif
        boot.dx12 = std::make_unique<smatchet::standalone::Dx12Bootstrap>();
        std::string dxErr;
        if (!boot.dx12->Init(hwnd, fbW, fbH, dxErr)) {
            LOG_ERROR("DX12 renderer init failed: %s", dxErr.c_str());
            return false;
        }
        LOG_INFO("DX12 renderer initialised (adapter: %s%s).", boot.dx12->AdapterDescription().c_str(),
                 boot.dx12->IsWarp() ? ", WARP software" : "");
        return true;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    glfwSetKeyCallback(window, KeypadEnterBridgeCallback);
    ImGui_ImplOpenGL3_Init(glsl_version);
    SmatchetLogOpenGLInfo();
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        ::fprintf(stderr, "Failed to create ImGui OpenGL device objects.\n"); // pre-logger-init — LOG_* unavailable
        return false;
    }
    return true;
}

// Boot the visible standalone app: crash-handler install, GLFW init, window
// creation + saved-position/maximize restore, ImGui context + style +
// platform/renderer init, CLI parse + ConfigManager::Load(cli) +
// InitAppAndPlugins. Fills `boot` (which WireUserDataAndLogSink already seeded
// with logPath) on success. Caller must have already wired the log sink + run
// the cmd/ephemeral short-circuits.
// Return contract: a non-negative result is an exit code that main returns
// immediately; the early-return and cleanup paths mirror the prior inline
// behaviour exactly, preserving glfwTerminate then ImGui shutdown order.
// A result of -1 means boot succeeded, so proceed to RunFrameLoop.
static int BootApplication(int argc, char** argv, MainBootState& boot) {
    // Resolve the renderer before any window hint is set — both the window-hint
    // choice (GL context vs GLFW_NO_API) and the backend init branch on it.
    boot.renderer = smatchet::standalone::ResolveStandaloneRenderer(argc, argv);

    const char* glsl_version = NULL;
    GLFWwindow* window = BootInitGlfwAndWindow(boot, glsl_version);
    if (window == NULL) {
        return 1;
    }

    if (!BootSetupImGui(window, glsl_version, boot)) {
        return 1;
    }

    ConfigManager::CliOverrides cli;
    if (!smatchet::standalone::ParseStandaloneCli(argc, argv, cli)) {
        glfwTerminate();
        return 0;
    }

    const TrackerConfig cfg = ConfigManager::Load(cli);

    // Seed the live vsync hub with the documented boot precedence and apply it
    // to the (current) GL context now; RunFrameLoop live-applies later changes.
    {
        const char* vsyncSource = smatchet::vsync::SeedFromBootSources(cfg.VsyncEnabled, cli.HasVsync, cli.Vsync);
        LOG_INFO("Vsync %s at boot (source: %s)", smatchet::vsync::Enabled() ? "enabled" : "disabled", vsyncSource);
        // GL applies vsync via the swap interval on the current context; DX12 has
        // no GL context and applies it per-frame in Present(syncInterval).
        if (boot.renderer == smatchet::standalone::StandaloneRenderer::OpenGL) {
            glfwSwapInterval(smatchet::vsync::Enabled() ? 1 : 0);
        }
    }

    BootstrapContext& bootCtx = boot.bootCtx;
    bootCtx.window = window;
    bootCtx.glslVersion = glsl_version;
    std::string bootErr;
    if (!smatchet::standalone::InitAppAndPlugins(bootCtx, cfg, false, bootErr)) {
        LOG_ERROR("App bootstrap failed: %s", bootErr.c_str());
        if (boot.renderer == smatchet::standalone::StandaloneRenderer::Dx12) {
            if (boot.dx12) {
                boot.dx12->Shutdown();
            }
        } else {
            ImGui_ImplOpenGL3_Shutdown();
        }
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    return -1; // boot succeeded — main() proceeds to RunFrameLoop
}

// Full screen toggle — requested by F11 handler in SmatchetUI::Draw.
// Saved windowed rect lives in function-local statics (singleton window → identical
// to the prior loop-body statics).
static void HandleFullScreenToggle(GLFWwindow* window) {
    static int s_windowedX = 100, s_windowedY = 100, s_windowedW = 1280, s_windowedH = 720;
    if (g_ui.requestFullScreenToggle) {
        g_ui.requestFullScreenToggle = false;
        g_ui.cfg.FullScreen = !g_ui.cfg.FullScreen;
        if (g_ui.cfg.FullScreen) {
            glfwGetWindowPos(window, &s_windowedX, &s_windowedY);
            glfwGetWindowSize(window, &s_windowedW, &s_windowedH);
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(mon);
            glfwSetWindowMonitor(window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            glfwSetWindowMonitor(window, nullptr, s_windowedX, s_windowedY, s_windowedW, s_windowedH, 0);
        }
    }
}

// Resize request from `debug.window.resize` (visual-test automation).
static void HandleResizeRequest(GLFWwindow* window) {
    if (g_ui.requestWindowResize) {
        g_ui.requestWindowResize = false;
        const int w = std::max(320, g_ui.requestWindowWidth);
        const int h = std::max(240, g_ui.requestWindowHeight);
        glfwSetWindowSize(window, w, h);
        LOG_INFO("debug.window.resize: %dx%d", w, h);
    }
}

// PackRgbDropAlpha lives in StandaloneBoot_detail (shared with the hidden-window
// bootstrap). Aliased so CaptureFrameRgb below calls it unqualified.
using boot_detail::PackRgbDropAlpha;

// Read the just-presented frame into top-left-origin RGB. GL reads the front
// buffer (bottom-left, flipped here); DX12 reads its swapchain back buffer
// (already top-left, no flip). Returns false on an empty framebuffer / failure.
static bool CaptureFrameRgb(GLFWwindow* window, MainBootState& boot, std::vector<unsigned char>& rgb, int& outW,
                            int& outH) {
    if (boot.renderer == smatchet::standalone::StandaloneRenderer::Dx12) {
        std::vector<uint8_t> rgba;
        int w = 0;
        int h = 0;
        if (!boot.dx12 || !boot.dx12->CaptureBackbuffer(rgba, w, h) || w <= 0 || h <= 0) {
            return false;
        }
        PackRgbDropAlpha(rgba.data(), w, h, /*flipVertical=*/false, rgb);
        outW = w;
        outH = h;
        return true;
    }

    int fw = 0;
    int fh = 0;
    glfwGetFramebufferSize(window, &fw, &fh);
    if (fw <= 0 || fh <= 0) {
        return false;
    }
    std::vector<unsigned char> pixels(static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4u);
    // Read from the front buffer; we have just SwapBuffers'd, so front == this frame.
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    PackRgbDropAlpha(pixels.data(), fw, fh, /*flipVertical=*/true, rgb);
    outW = fw;
    outH = fh;
    return true;
}

// Screenshot request from `debug.window.screenshot` — capture frame + write PNG
// via stb_image_write. PNG keeps tests/golden/* repo bloat down (~40× smaller than
// raw PPM-P6 on typical UI captures) and is readable by every image tool.
static void HandleScreenshotRequest(GLFWwindow* window, MainBootState& boot) {
    if (!g_ui.requestScreenshot) {
        return;
    }
    g_ui.requestScreenshot = false;
    // "Log a Bug" capture — own the staging dir + signal completion here (the
    // capture path is inherently main-thread and already writes to disk), so the
    // bug-report modal never does UI-thread filesystem I/O.
    const bool bugReportShot = g_ui.requestScreenshotBugReport;
    g_ui.requestScreenshotBugReport = false;
    const std::string screenshotPath = g_ui.requestScreenshotPath;
    if (bugReportShot && !screenshotPath.empty()) {
        std::error_code mkec;
        ghc::filesystem::create_directories(ghc::filesystem::path(screenshotPath).parent_path(), mkec);
    }
    int fw = 0;
    int fh = 0;
    std::vector<unsigned char> rgb;
    if (CaptureFrameRgb(window, boot, rgb, fw, fh) && fw > 0 && fh > 0 && !screenshotPath.empty()) {
        // "Log a Bug" capture: downscale so the base64 PNG fits the relay
        // payload cap. A full-size frame is several times too big. Text
        // redaction already happened at render time via the font swap, so
        // there is no CPU filter here. Gated on bugReportShot so golden and
        // test captures keep their native resolution.
        if (bugReportShot) {
            smatchet::imaging::DownscaleToMaxDimension(rgb, fw, fh, 3, 1280);
        }
        // Compression level 8 keeps capture cheap (~10 ms for a 1920x1080 frame on
        // dev hardware) while still cutting the file ~40× vs raw PPM. Stride is
        // recomputed from the (possibly downscaled) width.
        stbi_write_png_compression_level = 8;
        const int rc = stbi_write_png(screenshotPath.c_str(), fw, fh, 3, rgb.data(), fw * 3);
        if (rc == 0) {
            LOG_ERROR("debug.window.screenshot: stbi_write_png failed for %s", screenshotPath.c_str());
        } else {
            LOG_INFO("debug.window.screenshot: saved %s (%dx%d, PNG)", screenshotPath.c_str(), fw, fh);
        }
    } else {
        LOG_ERROR("debug.window.screenshot: invalid framebuffer or empty path");
    }
    // Signal the bug-report modal regardless of capture success — on failure the
    // staged PNG is absent and the worker degrades gracefully; either way the
    // modal's handshake must unblock (never leave bugReportInFlight stuck).
    if (bugReportShot) {
        g_ui.bugReportShotReady = true;
    }
}

// Pre-fill the bug-report modal from a crash marker left by the previous run.
static void MaybePrimeCrashReport() {
    if (!smatchet::diagnostics::CrashSinkHasPending()) {
        return;
    }
    const smatchet::diagnostics::CrashInfo ci = smatchet::diagnostics::CrashSinkConsume();
    std::string ctx = "Crash: " + ci.Reason;
    if (!ci.Breadcrumb.empty()) {
        ctx += "\nLast activity: " + ci.Breadcrumb;
    }
    if (!ci.DumpPath.empty()) {
        ctx += "\nMinidump: " + ci.DumpPath + " (uploaded on submit)";
    }
    if (!ci.LogTail.empty()) {
        ctx +=
            "\n\n<details><summary>Log tail (crashed session)</summary>\n\n```\n" + ci.LogTail + "\n```\n\n</details>";
    }
    g_ui.bugReportCrashContext = ctx;
    g_ui.bugReportCrashDumpPath = ci.DumpPath; // uploaded as a Release asset on submit
    g_ui.bugReportCrashMode = true;
    g_ui.showBugReport = true;
    g_ui.bugReportOpenLatch = true;
    LOG_WARN("Crash reporter: previous run crashed (%s) — opening pre-filled bug report.", ci.Reason.c_str());
}

// Render exactly one frame: poll events, apply any pending font reload, run the
// redaction-font swap window, build + draw the ImGui frame via the SEH-guarded
// bridge, clear with the active theme background, render the draw data, and
// present (plus the bucket-E post-swap hook). The renderer (GL vs DX12) is a
// runtime branch on boot.renderer — call order is load-bearing (steady-state
// render path); do not reorder. `syncInterval` is the resolved vsync interval
// (0/1) the DX12 path passes to Present each frame. Returns whether the
// redaction font was pushed this frame so the caller can pop it after the
// post-swap screenshot capture (the original ordering).
static bool RenderOneFrame(GLFWwindow* window, SmatchetUI& mainWindow, AppController& smatchetApp,
                           PluginHost& pluginHost, const ImVec4& clear_color, MainBootState& boot, int syncInterval) {
    // Poll and handle events (inputs, window resize, etc.)
    glfwPollEvents();

    if (SmatchetCheckAndApplyFontReload()) {
        // GL owns its font texture as a backend device object; the DX12 backend
        // rebuilds the atlas texture inside NewFrame, so only the GL path needs
        // the explicit destroy/recreate here.
        if (boot.renderer == smatchet::standalone::StandaloneRenderer::OpenGL) {
            ImGui_ImplOpenGL3_DestroyDeviceObjects();
            ImGui_ImplOpenGL3_CreateDeviceObjects();
        }
    }

    // font-redaction censor — if the bug-report modal armed a capture last frame,
    // swap the whole UI to the redaction font BEFORE NewFrame so this (captured)
    // frame renders all text as blocks. Popped after the screenshot capture below.
    const bool redactThisFrame = g_ui.requestRedactFontThisFrame;
    g_ui.requestRedactFontThisFrame = false;
    if (redactThisFrame) {
        SmatchetPushRedactionFonts();
    }

    // Start the ImGui frame
    if (boot.renderer == smatchet::standalone::StandaloneRenderer::Dx12) {
        boot.dx12->NewFrame();
    } else {
        ImGui_ImplOpenGL3_NewFrame();
    }
    ImGui_ImplGlfw_NewFrame();
    smatchet::ui::RouteWheelToScrollableTooltipBeforeNewFrame();
    ImGui::NewFrame();

    // THE BRIDGE: Hand control over to your engine-agnostic UI layer

    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);

    // Draw the main application
    SmatchetDrawFrameWithSeh(mainWindow, smatchetApp, pluginHost);

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    // Clear with the active theme's WindowBg so any dock gaps blend with panel
    // backgrounds — viewport background should never visibly differ from panels.
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    (void)clear_color;
    // pink-clear dock-gap scan (pink-clear-dock-gap-scan): a scenario may arm a
    // transient clear-color override for THIS frame so any uncovered dock gap
    // renders in the sentinel color (typically magenta 255,0,255) instead of the
    // theme background. TRANSIENT — read then clear the flag every frame (mirrors
    // the redaction-font swap); a scenario re-arms it per warm-up frame, so the
    // normal theme clear is restored the moment it stops re-arming. GL-only: the
    // GL renderer is the byte-stable bucket-C lane CI pins the goldens to; the DX12
    // BeginFrame clear keeps the theme bg.
    const bool clearOverride = g_ui.requestClearColorActive;
    const ImVec4 clearColor = clearOverride ? ImVec4(g_ui.requestClearColorR, g_ui.requestClearColorG,
                                                     g_ui.requestClearColorB, g_ui.requestClearColorA)
                                            : bg;
    g_ui.requestClearColorActive = false;
    ImDrawData* drawData = ImGui::GetDrawData();
    smatchet::ui::GuardImGuiDynamicTextures(drawData); // [P0 #1122] shared dynamic-texture guard
    if (boot.renderer == smatchet::standalone::StandaloneRenderer::Dx12) {
        // Swapchain resize tracks the framebuffer; skip rendering at 0x0 (minimised).
        boot.dx12->Resize(display_w, display_h);
        if (display_w > 0 && display_h > 0) {
            boot.dx12->BeginFrame(bg); // premultiplied clear inside, matching the GL path
            boot.dx12->RenderDrawData(drawData);
            boot.dx12->Present(syncInterval);
        }
    } else {
        glViewport(0, 0, display_w, display_h);
        glClearColor(clearColor.x * clearColor.w, clearColor.y * clearColor.w, clearColor.z * clearColor.w,
                     clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(drawData);
        glfwSwapBuffers(window);
    }

#if defined(SMATCHET_BUILD_UI_TESTS)
    // ImGui Test Engine bucket-E hookup. The engine is created /
    // destroyed by UiTestScenario, which publishes the active pointer
    // through the active-engine accessor. With no UI test running the
    // pointer is null and PostSwap is skipped — zero cost on the
    // normal frame path. See agents/test-author.md § Bucket E.
    if (ImGuiTestEngine* uiTestEngine = SmatchetActiveUiTestEngine()) {
        ImGuiTestEngine_PostSwap(uiTestEngine);
    }
#endif

    return redactThisFrame;
}

// Persist window state for next launch. Read maximized attrib directly; use the
// live-tracked windowed bounds so an exit-while-maximized still restores a sane
// windowed rect after un-maximize.
static void PersistWindowState(GLFWwindow* window, int persistWindowedX, int persistWindowedY, int persistWindowedW,
                               int persistWindowedH) {
    const bool maximized = (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0) || g_ui.cfg.FullScreen;
    TrackerConfig saveCfg = ConfigManager::Load();
    saveCfg.WindowX = persistWindowedX;
    saveCfg.WindowY = persistWindowedY;
    saveCfg.WindowWidth = persistWindowedW;
    saveCfg.WindowHeight = persistWindowedH;
    saveCfg.WindowMaximized = maximized;
    ConfigManager::Save(saveCfg);
}

// Run the main render loop until the window closes, then persist windowed
// bounds + tear down the app/plugin/window-owned UI via ShutdownApplication.
// ImGui/GLFW teardown stays with main() (mirrors the original inline ordering).
// Returns the process exit code (0 normal; 1 on a top-level exception).
// Env-gated autonomous FPS / frame-time measurement for the real GL window. When
// SMATCHET_FPS_MEASURE_SECONDS=N is set, samples per-frame wall time for N seconds (after a warmup
// that excludes startup/sync churn — SMATCHET_FPS_WARMUP_SECONDS, default 0.5 s), prints
// avg/best/p99/worst frame-ms + fps to stderr, then requests window close. SMATCHET_FPS_VSYNC=0
// disables vsync first to measure uncapped throughput. Zero cost when the env var is unset (one
// getenv at Begin + a single early-return branch per frame).
struct FpsMeasure {
    double measureSecs = 0.0;
    double warmupSecs = 0.5;
    double startT = 0.0;
    double lastT = 0.0;
    bool vsyncOff = false;
    bool done = false;
    std::vector<double> frameMs;

    void Begin(GLFWwindow* window) {
        (void)window;
        const char* m = std::getenv("SMATCHET_FPS_MEASURE_SECONDS");
        measureSecs = m ? std::atof(m) : 0.0;
        if (measureSecs <= 0.0) {
            return;
        }
        const char* w = std::getenv("SMATCHET_FPS_WARMUP_SECONDS");
        warmupSecs = w ? std::atof(w) : 0.5;
        // SMATCHET_FPS_VSYNC is applied at boot via vsync::SeedFromBootSources
        // (highest precedence) — read the hub here only for the report label.
        vsyncOff = !smatchet::vsync::Enabled();
        startT = glfwGetTime();
        lastT = startT;
    }

    void Sample(GLFWwindow* window) {
        if (measureSecs <= 0.0 || done) {
            return;
        }
        const double nowT = glfwGetTime();
        const double dtMs = (nowT - lastT) * 1000.0;
        lastT = nowT;
        if (nowT - startT > warmupSecs) {
            frameMs.push_back(dtMs);
        }
        if (nowT - startT < measureSecs || frameMs.empty()) {
            return;
        }
        std::sort(frameMs.begin(), frameMs.end());
        double sum = 0.0;
        for (double d : frameMs) {
            sum += d;
        }
        const size_t n = frameMs.size();
        const double avgMs = sum / static_cast<double>(n);
        const size_t p99i = (n * 99) / 100 >= n ? n - 1 : (n * 99) / 100;
        // Re-read the live vsync state at report time: the Preferences checkbox /
        // config.set can flip the hub mid-run, so the Begin snapshot may be stale.
        // Report "changed-mid-run" rather than a label that no longer reflects the
        // sampled window when the boot snapshot and the final state disagree.
        const bool vsyncOffNow = !smatchet::vsync::Enabled();
        const char* vsyncLabel = (vsyncOffNow != vsyncOff) ? "changed-mid-run" : (vsyncOff ? "off" : "on");
        ::fprintf(stderr,
                  "[fps-measure] frames=%zu vsync=%s | avg=%.2fms (%.0f fps) | best=%.2fms (%.0f fps) | "
                  "p99=%.2fms (%.0f fps) | worst=%.2fms (%.0f fps)\n",
                  n, vsyncLabel, avgMs, 1000.0 / avgMs, frameMs.front(), 1000.0 / frameMs.front(), frameMs[p99i],
                  1000.0 / frameMs[p99i], frameMs.back(), 1000.0 / frameMs.back());
        ::fflush(stderr);
        done = true;
        glfwSetWindowShouldClose(window, 1);
    }
};

// Env-gated one-shot scenario autostart: SMATCHET_AUTORUN_SCENARIO=<name> runs a
// registered scenario on the REAL GL window (vs --spawn, which is headless and
// defeats wall-clock FPS). Paired with SMATCHET_FPS_MEASURE_SECONDS the scenario
// injects interaction while FpsMeasure captures real FPS, then closes the window.
// Called once per loop with a latch — fires AFTER the first frame so the host pane
// machinery + live contexts exist. Direct Start() is safe: this IS the UI thread,
// between frames, so there is no race with ScenarioRunner::Tick (inside RenderOneFrame).
static void MaybeStartAutorunScenario(AppController& app, bool& started) {
    if (started) {
        return;
    }
    started = true;
    const char* autorunName = std::getenv("SMATCHET_AUTORUN_SCENARIO");
    if (!autorunName || autorunName[0] == '\0') {
        return;
    }
    smatchet::cmd::CommandContext autorunCtx;
    autorunCtx.ScenarioHost = &app;
    autorunCtx.Threading = &app;
    const smatchet::cmd::CommandResult autorunRes =
        app.Scenarios().Start(autorunName, nlohmann::json::object(), autorunCtx);
    ::fprintf(stderr, "[autorun-scenario] start name=%s ok=%d\n", autorunName, autorunRes.Ok ? 1 : 0);
    ::fflush(stderr);
}

static int RunFrameLoop(MainBootState& boot) {
    BootstrapContext& bootCtx = boot.bootCtx;
    GLFWwindow* window = bootCtx.window;
    const TrackerConfig& windowStateCfg = boot.windowStateCfg;
    const int initialWindowW = boot.initialWindowW;
    const int initialWindowH = boot.initialWindowH;
#if defined(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME)
    const bool restoreMaximized = boot.restoreMaximized;
#endif
    AppController& smatchetApp = *bootCtx.app;
    PluginHost& pluginHost = *bootCtx.pluginHost;

    int exitCode = 0;
    try {
        SmatchetUI mainWindow;

        // log-a-bug-github phase 2 — if the previous run left a crash marker, open the
        // bug-report modal pre-filled with the crash context so the user can file it.
        MaybePrimeCrashReport();

        // 4. The Main Render Loop
        ImVec4 clear_color = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

        // Live-tracked windowed bounds (non-maximized, non-fullscreen) — saved on exit
        // to restore on next launch. Seeded from saved config so an immediate maximize +
        // close still preserves a sensible windowed-restore rect.
        int s_persistWindowedX = (windowStateCfg.WindowX != -1) ? windowStateCfg.WindowX : 100;
        int s_persistWindowedY = (windowStateCfg.WindowY != -1) ? windowStateCfg.WindowY : 100;
        int s_persistWindowedW = initialWindowW;
        int s_persistWindowedH = initialWindowH;

        FpsMeasure fpsMeasure;
        fpsMeasure.Begin(window);

        // Latch for the one-shot SMATCHET_AUTORUN_SCENARIO start (see MaybeStartAutorunScenario).
        bool autorunScenarioStarted = false;

        // Live vsync apply — mirrors the SmatchetUI lastAppliedTheme_ pattern:
        // one atomic load + compare per frame; glfwSwapInterval only on change
        // (Preferences checkbox / config.set vsync flip the hub at runtime).
        bool lastAppliedVsync = smatchet::vsync::Enabled();

        while (!glfwWindowShouldClose(window)) {
            // Feed the OS window-focus signal to the ticket-change monitor (plan item 17): a
            // backgrounded window stops polling for changes. GLFW_FOCUSED is cheap and valid
            // every frame; the no-signal default on the AppController side is true.
            smatchetApp.SetWindowFocused(glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0);

            const bool liveVsync = smatchet::vsync::Enabled();
            const int syncInterval = liveVsync ? 1 : 0;
            // GL applies vsync via glfwSwapInterval on-change; DX12 takes syncInterval
            // per-frame into Present() (inside RenderOneFrame), so only the GL path
            // touches the GL context here.
            if (boot.renderer == smatchet::standalone::StandaloneRenderer::OpenGL && liveVsync != lastAppliedVsync) {
                glfwSwapInterval(liveVsync ? 1 : 0);
                lastAppliedVsync = liveVsync;
            }

            const bool redactThisFrame =
                RenderOneFrame(window, mainWindow, smatchetApp, pluginHost, clear_color, boot, syncInterval);

            // Fire the env-gated autostart scenario once, after the first frame.
            MaybeStartAutorunScenario(smatchetApp, autorunScenarioStarted);

            fpsMeasure.Sample(window);

            // Snapshot windowed bounds whenever the window is in a normal (non-maximized,
            // non-fullscreen) state so we can persist them on exit.
            if (!g_ui.cfg.FullScreen && glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == 0) {
                glfwGetWindowPos(window, &s_persistWindowedX, &s_persistWindowedY);
                glfwGetWindowSize(window, &s_persistWindowedW, &s_persistWindowedH);
            }

            HandleFullScreenToggle(window);
            HandleResizeRequest(window);
            HandleScreenshotRequest(window, boot);

            // Restore normal fonts after the redacted capture frame (next frame is normal).
            if (redactThisFrame) {
                SmatchetPopRedactionFonts();
            }

#if defined(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME)
            if (!g_MainWindowShownAfterFirstFrame) {
                glfwShowWindow(window);
                // GLFW_MAXIMIZED hint can be lost across the hidden->shown transition on
                // some Windows configs; re-apply explicitly so saved-maximized restores.
                if (restoreMaximized && glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == 0) {
                    glfwMaximizeWindow(window);
                }
                g_MainWindowShownAfterFirstFrame = true;
            }
#endif
        }

        PersistWindowState(window, s_persistWindowedX, s_persistWindowedY, s_persistWindowedW, s_persistWindowedH);

        smatchet::standalone::ShutdownApplication(bootCtx);
    } catch (const std::exception& ex) {
        smatchet::standalone::ShutdownApplication(bootCtx);
        ::fprintf(stderr, "Exception caught in entry point: %s\n", ex.what()); // pre-logger-init — LOG_* unavailable
        exitCode = 1;
    } catch (...) { // catch-all-ok: top-level entry-point reporter — fprintf below (logger unavailable at shutdown)
        smatchet::standalone::ShutdownApplication(bootCtx);
        ::fprintf(stderr, "Unknown exception caught in entry point.\n"); // pre-logger-init — LOG_* unavailable
        exitCode = 1;
    }

    // 5. Cleanup
    if (boot.renderer == smatchet::standalone::StandaloneRenderer::Dx12) {
        if (boot.dx12) {
            boot.dx12->Shutdown();
        }
    } else {
        ImGui_ImplOpenGL3_Shutdown();
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return exitCode;
}

} // namespace
} // namespace standalone
} // namespace smatchet

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Security (startup hardening): restrict the implicit DLL search order to
    // System32 before any library is dynamically loaded. This drops the current
    // working directory and the application directory from the default
    // LoadLibrary search path, closing the classic DLL-planting / search-order
    // hijack vector at process start. Must be the first statement in main() so it
    // precedes every subsequent runtime load. SetDefaultDllDirectories lives in
    // kernel32 (always linked) and is declared by <windows.h>, included above.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
#endif

    smatchet::standalone::MainBootState boot;

    // Resolve user-data + wire the log sink first so the cmd/ephemeral
    // short-circuits below inherit them, exactly as the original prologue did.
    smatchet::standalone::WireUserDataAndLogSink(boot);

    // Unified Command System — when the first non-flag positional after the
    // program name is `cmd`, short-circuit the GUI boot and run as an HTTP
    // client to a running Smatchet instance. This is the agent-friendly path
    // (see docs/plans/active/applied/command-system-plan.md §CLI). All output is structured JSON
    // on stdout / errors on stderr; no GLFW / ImGui init happens in this mode.
    if (smatchet::cli::ArgvHasCmdSubcommand(argc, argv)) {
        return smatchet::cli::RunCmdAttach(argc, argv);
    }

    if (smatchet::cli::IsEphemeralMode(argc, argv)) {
        std::string bootErr;
        if (!smatchet::standalone::BootEphemeral(argc, argv, bootErr)) {
            if (!bootErr.empty()) {
                std::fprintf(stderr, "%s\n", bootErr.c_str()); // pre-logger-init — LOG_* unavailable
            }
            return 1;
        }
        return 0;
    }

    const int bootRc = smatchet::standalone::BootApplication(argc, argv, boot);
    if (bootRc >= 0) {
        return bootRc; // early-return / cleanup path already handled inside BootApplication
    }

    return smatchet::standalone::RunFrameLoop(boot);
}
