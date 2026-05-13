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
// AppController holds unique_ptr<> members of these service types; main.cpp's
// stack instance triggers destructor / noexcept evaluation that requires the
// complete types, so include them here (CODE_REVIEW items 11/12/13/14).
#include "ITrackerBackendFactory.h"
#include "LuaAutomationHost.h"
#include "OfflineQueueService.h"
#include "TicketSyncService.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "PluginHost.h"
#include "SmatchetUI.h"
#if defined(SMATCHET_WITH_MCP)
#include "McpPlugin.h"
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
#include "LuaConsolePlugin.h"
#endif

#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif

// UiDrawSession is defined in SmatchetUI.cpp (translation-unit global g_ui).
// main.cpp reads requestFullScreenToggle and cfg.FullScreen after each frame.
#include "SmatchetUiSession.h"
extern UiDrawSession g_ui;

// GLFW Error Callback
static void glfw_error_callback(int error, const char* description) {
    ::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
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
            ::fprintf(stderr, "Icon not found in resources (RT_GROUP_ICON) id=%u\n", (unsigned)kIconResourceId);
        } else {
            ::fprintf(stderr, "Icon resource exists but LoadIcon/LoadImage failed id=%u\n", (unsigned)kIconResourceId);
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
    } catch (...) {
        throw;
    }
}

// MSVC: __try cannot appear in a function that needs C++ object unwinding (e.g. main with PluginHost on stack).
static void SmatchetDrawFrameWithSeh(SmatchetUI& mainWindow, AppController& smatchetApp, PluginHost& pluginHost) {
#if defined(_WIN32) && defined(_MSC_VER)
    __try {
        SmatchetDrawFrame(mainWindow, smatchetApp, pluginHost);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    ::fprintf(stderr, "OpenGL vendor: %s\n", vendor ? reinterpret_cast<const char*>(vendor) : "(null)");
    ::fprintf(stderr, "OpenGL renderer: %s\n", renderer ? reinterpret_cast<const char*>(renderer) : "(null)");
    ::fprintf(stderr, "OpenGL version: %s\n", version ? reinterpret_cast<const char*>(version) : "(null)");
    ::fprintf(stderr, "OpenGL GLSL: %s\n", shading ? reinterpret_cast<const char*>(shading) : "(null)");
}

static std::string SmatchetNormalizeDirectory(std::string path) {
    if (path.empty()) {
        return path;
    }
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
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

static std::string SmatchetGetStandaloneUserDataDirectory() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return SmatchetNormalizeDirectory(std::string(buf) + "\\Smatchet");
    }
    n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return SmatchetNormalizeDirectory(std::string(buf) + "\\Smatchet");
    }
    return std::string();
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return SmatchetNormalizeDirectory(std::string(home) + "/Library/Application Support/Smatchet");
    }
    return std::string();
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return SmatchetNormalizeDirectory(std::string(xdg) + "/Smatchet");
    }
    if (const char* home = std::getenv("HOME")) {
        return SmatchetNormalizeDirectory(std::string(home) + "/.config/Smatchet");
    }
    return std::string();
#endif
}

int main(int argc, char** argv) {
    // SMATCHET_USER_DATA — override the writable user data directory.
    // Applied here, before ConfigManager::Load(), so the config file, SQLite DB,
    // views, recents, and instance.json all resolve under the overridden path.
    // Stable API: see backlog/COMMAND_SYSTEM_PLAN.md §"Environment contract".
    {
        const char* envUserData = std::getenv("SMATCHET_USER_DATA");
        if (envUserData && envUserData[0] != '\0') {
            const std::string userDataDir = SmatchetNormalizeDirectory(std::string(envUserData));
            SmatchetEnsureDirectoryExists(userDataDir);
            ConfigManager::SetUserDataDirectory(userDataDir);
        }
    }

    // Unified Command System — when the first non-flag positional after the
    // program name is `cmd`, short-circuit the GUI boot and run as an HTTP
    // client to a running Smatchet instance. This is the agent-friendly path
    // (see backlog/COMMAND_SYSTEM_PLAN.md §CLI). All output is structured JSON
    // on stdout / errors on stderr; no GLFW / ImGui init happens in this mode.
    if (smatchet::cli::ArgvHasCmdSubcommand(argc, argv)) {
        return smatchet::cli::RunCmdAttach(argc, argv);
    }

    // --ephemeral: launched by the CLI's --spawn mechanism for automated testing.
    // Run normally (full app init, MCP server) but start with a hidden window so
    // there is no visible UI. The CLI will send app.quit when it's done.
    const bool ephemeralMode = smatchet::cli::IsEphemeralMode(argc, argv);

    // 1. Setup OS Window (GLFW)
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

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
    if (ephemeralMode) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Smatchet - Standalone", NULL, NULL);
    if (window == NULL) {
        return 1;
    }

#if defined(_WIN32)
    SmatchetApplyWindowIcon(window);
#endif

    glfwMakeContextCurrent(window);

    glfwSwapInterval(1); // Enable vsync

    // Separate shipped runtime assets (next to the exe) from writable user data.
    // Helper: resolve userDataDir given the exe dir. Honors `smatchet_storage_mode.txt`
    // alongside the exe. Standalone defaults to `Shared` (OS user-data dir) when no
    // explicit marker exists. The marker is authoritative across launches; the
    // preferences toggle just writes the new value and the next launch picks it up.
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

    // Setup ImGui Style
    ImGui::StyleColorsDark();
    SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io);

    // Setup Platform/Renderer bindings
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    glfwSetKeyCallback(window, SmatchetKeypadEnterBridgeCallback);
    ImGui_ImplOpenGL3_Init(glsl_version);
    SmatchetLogOpenGLInfo();
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        ::fprintf(stderr, "Failed to create ImGui OpenGL device objects.\n");
        return 1;
    }

    // 3. Load merged configuration with command-line overrides
    ConfigManager::CliOverrides cli;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            ::printf("Smatchet Standalone Client\n");
            ::printf("Usage: SmatchetStandalone [options]\n");
            ::printf("Options:\n");
            ::printf("  -d, --db-path <path>         Path to sqlite database\n");
            ::printf("  -b, --backend-type <type>    Tracker type ('Jira' or 'Plane')\n");
            ::printf("  -p, --mcp-port <port>        MCP server port number\n");
            ::printf("      --mcp-allow-remote       Allow remote connections to MCP server\n");
            ::printf("  -h, --help                   Show help message\n");
            glfwTerminate();
            return 0;
        } else if ((arg == "--db-path" || arg == "-d") && i + 1 < argc) {
            cli.HasDbPath = true;
            cli.DbPath = argv[++i];
        } else if ((arg == "--backend-type" || arg == "--tracker-type" || arg == "-b") && i + 1 < argc) {
            cli.HasBackendType = true;
            cli.BackendType = argv[++i];
        } else if ((arg == "--mcp-port" || arg == "-p") && i + 1 < argc) {
            try {
                cli.HasMcpPort = true;
                cli.McpPort = std::stoi(argv[++i]);
            } catch (...) {}
        } else if (arg == "--mcp-allow-remote") {
            cli.HasMcpAllowRemote = true;
            cli.McpAllowRemote = true;
        }
    }

    const TrackerConfig cfg = ConfigManager::Load(cli);

    int exitCode = 0;
    try {
        AppController smatchetApp;
        PluginHost pluginHost;
        smatchetApp.SetRuntimePluginHost(&pluginHost);
        smatchetApp.SetRequestAppQuitHandler([window]() {
            if (window) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        });
#if defined(SMATCHET_WITH_MCP)
        {
            // Ephemeral spawn mode forces MCP on regardless of user config, since the spawning
            // CLI process needs the HTTP endpoint to dispatch commands and request app.quit.
            const bool wantMcp = cfg.McpEnabled || ephemeralMode;
            if (wantMcp) {
                const int mcpPort =
                    (cfg.McpPort >= 1 && cfg.McpPort <= 65535) ? cfg.McpPort : SmatchetDefaults::Mcp::kDefaultPort;
                pluginHost.Register(std::make_unique<McpPlugin>(mcpPort));
            }
        }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        pluginHost.Register(std::make_unique<LuaConsolePlugin>());
#endif
        pluginHost.OnEarlyInit(smatchetApp);

        smatchetApp.Initialize(cfg.DbPath, cfg.TrackerType);
        pluginHost.OnStart(smatchetApp);

        SmatchetUI mainWindow;

        // 4. The Main Render Loop
        ImVec4 clear_color = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

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

            // Full-screen dockspace — NoUndocking matches SmatchetImGuiHost.cpp (DX12 path).
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_NoUndocking);

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

            // Screenshot request from `debug.window.screenshot` — read framebuffer + write PPM.
            // PPM is chosen over PNG because stb_image_write is not linked into the standalone
            // target; PPM is uncompressed but readable by every image tool and by the harness.
            if (g_ui.requestScreenshot) {
                g_ui.requestScreenshot = false;
                const std::string screenshotPath = g_ui.requestScreenshotPath;
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(window, &fw, &fh);
                if (fw > 0 && fh > 0 && !screenshotPath.empty()) {
                    std::vector<unsigned char> pixels(static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4u);
                    // Read from the back buffer; we have just SwapBuffers'd, so front == previous frame.
                    // GL_BACK after swap holds the freshly-presented frame on most drivers; use GL_FRONT
                    // since we've already swapped — the front buffer is the most recently shown image.
                    glReadBuffer(GL_FRONT);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                    // Flip vertically: OpenGL origin is bottom-left, image files expect top-left.
                    const size_t rowBytes = static_cast<size_t>(fw) * 4u;
                    std::vector<unsigned char> flipped(pixels.size());
                    for (int y = 0; y < fh; ++y) {
                        std::memcpy(
                            &flipped[static_cast<size_t>(y) * rowBytes],
                            &pixels[static_cast<size_t>(fh - 1 - y) * rowBytes],
                            rowBytes);
                    }
                    FILE* fp = std::fopen(screenshotPath.c_str(), "wb");
                    if (!fp) {
                        LOG_ERROR("debug.window.screenshot: cannot open %s for writing",
                                  screenshotPath.c_str());
                    } else {
                        std::fprintf(fp, "P6\n%d %d\n255\n", fw, fh);
                        for (int y = 0; y < fh; ++y) {
                            for (int x = 0; x < fw; ++x) {
                                const unsigned char* p =
                                    &flipped[(static_cast<size_t>(y) * static_cast<size_t>(fw) +
                                              static_cast<size_t>(x)) *
                                             4u];
                                std::fputc(p[0], fp);
                                std::fputc(p[1], fp);
                                std::fputc(p[2], fp);
                            }
                        }
                        std::fclose(fp);
                        LOG_INFO("debug.window.screenshot: saved %s (%dx%d, PPM)",
                                 screenshotPath.c_str(), fw, fh);
                    }
                } else {
                    LOG_ERROR("debug.window.screenshot: invalid framebuffer or empty path");
                }
            }

#if defined(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME)
            if (!g_MainWindowShownAfterFirstFrame) {
                glfwShowWindow(window);
                g_MainWindowShownAfterFirstFrame = true;
            }
#endif
        }

        smatchetApp.ClearAutomationLogSinks();
        smatchetApp.SetRuntimePluginHost(nullptr);
        pluginHost.OnStop();
    } catch (const std::exception& ex) {
        ::fprintf(stderr, "Exception caught in entry point: %s\n", ex.what());
        exitCode = 1;
    } catch (...) {
        ::fprintf(stderr, "Unknown exception caught in entry point.\n");
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


