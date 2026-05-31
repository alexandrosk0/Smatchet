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
//
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
#pragma warning(push)
#pragma warning(disable : 4996) // stb uses sprintf — third-party, cross-platform pragma suppression
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
#include "AppController.h"
#include "CliCommandRunner.h"
#include "StandaloneAppBootstrap.h"
// AppController holds unique_ptr<> members of these service types; main.cpp's
// stack instance triggers destructor / noexcept evaluation that requires the
// complete types, so include them here (CODE_REVIEW items 11/12/13/14).
#include "AppControllerDepsAdapter.h"
#include "ITrackerBackendFactory.h"
#include "LuaAutomationHost.h"
#include "OfflineQueueService.h"
#include "TicketSyncService.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "PluginHost.h"
#include "SmatchetUI.h"

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

// GLFW Key Callback wrapper to bridge Keypad Enter to standard Enter key
static void SmatchetKeypadEnterBridgeCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key == GLFW_KEY_KP_ENTER) {
        ImGui_ImplGlfw_KeyCallback(window, GLFW_KEY_ENTER, scancode, action, mods);
    }
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
}

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

static std::string SmatchetNormalizeDirectory(std::string path) {
    if (path.empty()) {
        return path;
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.back() != '/') {
        path.push_back('/');
    }
    return path;
}

static void SmatchetEnsureDirectoryExists(const std::string& path) {
    namespace fs = ghc::filesystem;
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
}

int main(int argc, char** argv) {
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
            const std::string userDataDir = SmatchetNormalizeDirectory(std::string(envUserData));
            SmatchetEnsureDirectoryExists(userDataDir);
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
    //
    // Resolution order: SMATCHET_DEBUG_LOG env wins (operator override); else
    // %LOCALAPPDATA%\Smatchet\Smatchet-<pid>.log on Windows / $TMPDIR fallback.
    // The per-PID suffix means concurrent Smatchet processes (e.g. spawn-mode
    // ephemeral + the user's manual instance) get distinct log files instead
    // of interleaving + corrupting a shared sink.
    std::string logPath; // hoisted so the crash reporter can record it for next-launch log-tail
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
                SmatchetEnsureDirectoryExists(dir);
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

    // 1. Setup OS Window (GLFW)
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

    // Resolve user-data dir BEFORE creating the GLFW window so the saved window
    // pos/size/maximized state in config can drive the initial window hints.
    const auto resolveStandaloneUserDataDir = [](const std::string& exeDir) -> std::string {
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
    };

#if defined(_WIN32)
    {
        char buf[MAX_PATH];
        if (GetModuleFileNameA(NULL, buf, MAX_PATH)) {
            std::string exePath(buf);
            std::string::size_type last = exePath.find_last_of("\\/");
            if (last != std::string::npos) {
                std::string exeDir = SmatchetNormalizeDirectory(exePath.substr(0, last + 1));
                ConfigManager::SetRuntimeAssetDirectory(exeDir);
                std::string userDataDir = resolveStandaloneUserDataDir(exeDir);
                SmatchetEnsureDirectoryExists(userDataDir);
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
                const std::string exeDir = SmatchetNormalizeDirectory(exePath.substr(0, last + 1));
                ConfigManager::SetRuntimeAssetDirectory(exeDir);
                std::string userDataDir = resolveStandaloneUserDataDir(exeDir);
                SmatchetEnsureDirectoryExists(userDataDir);
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
                const std::string exeDir = SmatchetNormalizeDirectory(exePath.substr(0, last + 1));
                ConfigManager::SetRuntimeAssetDirectory(exeDir);
                std::string userDataDir = resolveStandaloneUserDataDir(exeDir);
                SmatchetEnsureDirectoryExists(userDataDir);
                ConfigManager::SetUserDataDirectory(userDataDir);
            }
        }
    }
#endif

    // log-a-bug-github phase 2 — install crash handlers now that the user-data dir is
    // resolved (CrashSinkInit writes its marker/dump there). Runs before any heavy work
    // so startup crashes are still captured. CrashSinkInit builds the static path buffers
    // the async handlers use; InstallCrashHandlers wires SEH/terminate/signals.
    smatchet::diagnostics::CrashSinkInit(ConfigManager::GetUserDataDirectory(), logPath);
    smatchet::InstallCrashHandlers();
    smatchet::diagnostics::CrashSinkBreadcrumb("startup");

    // Peek window state from saved config so we can hint MAXIMIZED + initial size.
    const TrackerConfig windowStateCfg = ConfigManager::Load();
    const int initialWindowW = std::max(320, windowStateCfg.WindowWidth);
    const int initialWindowH = std::max(240, windowStateCfg.WindowHeight);
    const bool restoreMaximized = windowStateCfg.WindowMaximized;
    const bool havePosHint = (windowStateCfg.WindowX != -1 && windowStateCfg.WindowY != -1);

    // Decide GL+GLSL versions
#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

#if defined(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#endif
    if (restoreMaximized) {
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    }

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(initialWindowW, initialWindowH, "Smatchet - Standalone", NULL, NULL);
    if (window == NULL) {
        return 1;
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

    glfwMakeContextCurrent(window);

    glfwSwapInterval(1); // Enable vsync

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
    //
    // StyleColorsDark seeds ImGui's default-dark substrate (#FFFFFF text,
    // #4296FA blue-on-translucent button hover, FramePadding [4,3], rounding
    // 0). SmatchetTheme::ApplyStyle(SmatchetDark) is what actually paints the
    // Smatchet look — #F2F2F2 text on #1F1F24 panels, the #383842 button base
    // with #598CF2 accent, FramePadding [6,4], the 4-6 rounding family.
    //
    ImGui::StyleColorsDark();
    SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io);

    // Setup Platform/Renderer bindings
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    glfwSetKeyCallback(window, SmatchetKeypadEnterBridgeCallback);
    ImGui_ImplOpenGL3_Init(glsl_version);
    SmatchetLogOpenGLInfo();
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        ::fprintf(stderr, "Failed to create ImGui OpenGL device objects.\n"); // pre-logger-init — LOG_* unavailable
        return 1;
    }

    ConfigManager::CliOverrides cli;
    if (!smatchet::standalone::ParseStandaloneCli(argc, argv, cli)) {
        glfwTerminate();
        return 0;
    }

    const TrackerConfig cfg = ConfigManager::Load(cli);

    smatchet::standalone::BootstrapContext bootCtx;
    bootCtx.window = window;
    bootCtx.glslVersion = glsl_version;
    std::string bootErr;
    if (!smatchet::standalone::InitAppAndPlugins(bootCtx, cfg, false, bootErr)) {
        LOG_ERROR("App bootstrap failed: %s", bootErr.c_str());
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    AppController& smatchetApp = *bootCtx.app;
    PluginHost& pluginHost = *bootCtx.pluginHost;

    int exitCode = 0;
    try {
        SmatchetUI mainWindow;

        // log-a-bug-github phase 2 — if the previous run left a crash marker, open the
        // bug-report modal pre-filled with the crash context so the user can file it.
        if (smatchet::diagnostics::CrashSinkHasPending()) {
            const smatchet::diagnostics::CrashInfo ci = smatchet::diagnostics::CrashSinkConsume();
            std::string ctx = "Crash: " + ci.Reason;
            if (!ci.Breadcrumb.empty()) {
                ctx += "\nLast activity: " + ci.Breadcrumb;
            }
            if (!ci.DumpPath.empty()) {
                ctx += "\nMinidump: " + ci.DumpPath + " (uploaded on submit)";
            }
            if (!ci.LogTail.empty()) {
                ctx += "\n\n<details><summary>Log tail (crashed session)</summary>\n\n```\n" + ci.LogTail +
                       "\n```\n\n</details>";
            }
            g_ui.bugReportCrashContext = ctx;
            g_ui.bugReportCrashDumpPath = ci.DumpPath; // uploaded as a Release asset on submit
            g_ui.bugReportCrashMode = true;
            g_ui.showBugReport = true;
            g_ui.bugReportOpenLatch = true;
            LOG_WARN("Crash reporter: previous run crashed (%s) — opening pre-filled bug report.", ci.Reason.c_str());
        }

        // 4. The Main Render Loop
        ImVec4 clear_color = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

        // Live-tracked windowed bounds (non-maximized, non-fullscreen) — saved on exit
        // to restore on next launch. Seeded from saved config so an immediate maximize +
        // close still preserves a sensible windowed-restore rect.
        int s_persistWindowedX = (windowStateCfg.WindowX != -1) ? windowStateCfg.WindowX : 100;
        int s_persistWindowedY = (windowStateCfg.WindowY != -1) ? windowStateCfg.WindowY : 100;
        int s_persistWindowedW = initialWindowW;
        int s_persistWindowedH = initialWindowH;

        while (!glfwWindowShouldClose(window)) {
            // Poll and handle events (inputs, window resize, etc.)
            glfwPollEvents();

            if (SmatchetCheckAndApplyFontReload()) {
                ImGui_ImplOpenGL3_DestroyDeviceObjects();
                ImGui_ImplOpenGL3_CreateDeviceObjects();
            }

            // Start the ImGui frame
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // ====================================================================
            // THE BRIDGE: Hand control over to your engine-agnostic UI layer
            // ====================================================================

            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);

            // Draw the main application
            SmatchetDrawFrameWithSeh(mainWindow, smatchetApp, pluginHost);

            // Rendering
            ImGui::Render();
            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            // Clear with the active theme's WindowBg so any dock gaps blend with panel
            // backgrounds — viewport background should never visibly differ from panels.
            const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
            glClearColor(bg.x * bg.w, bg.y * bg.w, bg.z * bg.w, bg.w);
            glClear(GL_COLOR_BUFFER_BIT);
            (void)clear_color;
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);

#if defined(SMATCHET_BUILD_UI_TESTS)
            // ImGui Test Engine bucket-E hookup. The engine is created /
            // destroyed by UiTestScenario, which publishes the active pointer
            // via SmatchetActiveUiTestEngine(). When no UI test is running the
            // pointer is null and PostSwap is skipped — zero cost on the
            // normal frame path. See agents/test-author.md § Bucket E.
            if (ImGuiTestEngine* uiTestEngine = SmatchetActiveUiTestEngine()) {
                ImGuiTestEngine_PostSwap(uiTestEngine);
            }
#endif

            // Snapshot windowed bounds whenever the window is in a normal (non-maximized,
            // non-fullscreen) state so we can persist them on exit.
            if (!g_ui.cfg.FullScreen && glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == 0) {
                glfwGetWindowPos(window, &s_persistWindowedX, &s_persistWindowedY);
                glfwGetWindowSize(window, &s_persistWindowedW, &s_persistWindowedH);
            }

            // Full screen toggle — requested by F11 handler in SmatchetUI::Draw.
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

            // Resize request from `debug.window.resize` (visual-test automation).
            if (g_ui.requestWindowResize) {
                g_ui.requestWindowResize = false;
                const int w = std::max(320, g_ui.requestWindowWidth);
                const int h = std::max(240, g_ui.requestWindowHeight);
                glfwSetWindowSize(window, w, h);
                LOG_INFO("debug.window.resize: %dx%d", w, h);
            }

            // Screenshot request from `debug.window.screenshot` — read framebuffer + write PNG
            // via stb_image_write. PNG keeps tests/golden/* repo bloat down (~40× smaller than
            // raw PPM-P6 on typical UI captures) and is readable by every image tool.
            if (g_ui.requestScreenshot) {
                g_ui.requestScreenshot = false;
                const bool censorThisShot = g_ui.requestScreenshotCensor;
                g_ui.requestScreenshotCensor = false;
                const int censorBlockOverride = g_ui.requestScreenshotCensorBlock; // 0 = auto
                g_ui.requestScreenshotCensorBlock = 0;
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
                glfwGetFramebufferSize(window, &fw, &fh);
                if (fw > 0 && fh > 0 && !screenshotPath.empty()) {
                    std::vector<unsigned char> pixels(static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4u);
                    // Read from the back buffer; we have just SwapBuffers'd, so front == previous frame.
                    glReadBuffer(GL_FRONT);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                    // Flip vertically: OpenGL origin is bottom-left, image files expect top-left.
                    // Drop the alpha channel — golden diffs only compare R/G/B.
                    const size_t rowBytesRgb = static_cast<size_t>(fw) * 3u;
                    std::vector<unsigned char> rgb(static_cast<size_t>(fh) * rowBytesRgb);
                    for (int y = 0; y < fh; ++y) {
                        const unsigned char* src =
                            &pixels[static_cast<size_t>(fh - 1 - y) * static_cast<size_t>(fw) * 4u];
                        unsigned char* dst = &rgb[static_cast<size_t>(y) * rowBytesRgb];
                        for (int x = 0; x < fw; ++x) {
                            dst[0] = src[0];
                            dst[1] = src[1];
                            dst[2] = src[2];
                            dst += 3;
                            src += 4;
                        }
                    }
                    // "Log a Bug" capture: downscale so the base64 PNG fits the relay
                    // payload cap (a 1920px frame is ~4x too big), THEN censor. Gated on
                    // bugReportShot so golden/test captures keep their native resolution.
                    if (bugReportShot) {
                        smatchet::imaging::DownscaleToMaxDimension(rgb, fw, fh, 3, 1280);
                    }
                    // "Log a Bug" censored variant — mosaic the frame so no text is
                    // readable before it is written/uploaded. Consumed once per request.
                    if (censorThisShot) {
                        const int block = censorBlockOverride > 0 ? censorBlockOverride
                                                                  : smatchet::imaging::RecommendedCensorBlock(fw, fh);
                        smatchet::imaging::MosaicCensorInPlace(rgb.data(), fw, fh, 3, block);
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

        // Persist window state for next launch. Read maximized attrib directly; use the
        // live-tracked windowed bounds so an exit-while-maximized still restores a sane
        // windowed rect after un-maximize.
        {
            const bool maximized = (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0) || g_ui.cfg.FullScreen;
            TrackerConfig saveCfg = ConfigManager::Load();
            saveCfg.WindowX = s_persistWindowedX;
            saveCfg.WindowY = s_persistWindowedY;
            saveCfg.WindowWidth = s_persistWindowedW;
            saveCfg.WindowHeight = s_persistWindowedH;
            saveCfg.WindowMaximized = maximized;
            ConfigManager::Save(saveCfg);
        }

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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return exitCode;
}
