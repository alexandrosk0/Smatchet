#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include "StandaloneAppBootstrap.h"

#include "AppController.h"
#include "AppControllerDepsAdapter.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "ITrackerBackendFactory.h"
#include "LuaAutomationHost.h"
#include "OfflineQueueService.h"
#include "PluginHost.h"
#include "TicketSyncService.h"
#include "Commands/Scenarios/IScenario.h"
#include "SmatchetDefaults.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetUI.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
#include "LuaConsolePlugin.h"
#endif
#if defined(SMATCHET_WITH_MCP)
#include "McpPlugin.h"
#endif
#if defined(SMATCHET_WITH_WHISPER)
#include "WhisperPlugin.h"
#endif

extern UiDrawSession g_ui;

namespace smatchet {
namespace standalone {

namespace {

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
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    ghc::filesystem::create_directories(ghc::filesystem::path(path), ec);
}

static void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void SmatchetKeypadEnterBridgeCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key == GLFW_KEY_KP_ENTER) {
        ImGui_ImplGlfw_KeyCallback(window, GLFW_KEY_ENTER, scancode, action, mods);
    }
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
}

static std::string ResolveStandaloneUserDataDir(const std::string& exeDir) {
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

static void SetupRuntimePaths() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetModuleFileNameA(NULL, buf, MAX_PATH)) {
        std::string exePath(buf);
        const std::string::size_type last = exePath.find_last_of("\\/");
        if (last != std::string::npos) {
            const std::string exeDir = SmatchetNormalizeDirectory(exePath.substr(0, last + 1));
            ConfigManager::SetRuntimeAssetDirectory(exeDir);
            const std::string userDataDir = ResolveStandaloneUserDataDir(exeDir);
            SmatchetEnsureDirectoryExists(userDataDir);
            ConfigManager::SetUserDataDirectory(userDataDir);
        }
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string exePath(buf);
        const std::string::size_type last = exePath.find_last_of('/');
        if (last != std::string::npos) {
            const std::string exeDir = SmatchetNormalizeDirectory(exePath.substr(0, last + 1));
            ConfigManager::SetRuntimeAssetDirectory(exeDir);
            const std::string userDataDir = ResolveStandaloneUserDataDir(exeDir);
            SmatchetEnsureDirectoryExists(userDataDir);
            ConfigManager::SetUserDataDirectory(userDataDir);
        }
    }
#elif defined(__APPLE__)
    std::vector<char> buf(PATH_MAX);
    uint32_t sz = static_cast<uint32_t>(buf.size());
    if (_NSGetExecutablePath(buf.data(), &sz) == 0) {
        std::string exePath(buf.data());
        const std::string::size_type last = exePath.find_last_of('/');
        if (last != std::string::npos) {
            const std::string exeDir = SmatchetNormalizeDirectory(exePath.substr(0, last + 1));
            ConfigManager::SetRuntimeAssetDirectory(exeDir);
            const std::string userDataDir = ResolveStandaloneUserDataDir(exeDir);
            SmatchetEnsureDirectoryExists(userDataDir);
            ConfigManager::SetUserDataDirectory(userDataDir);
        }
    }
#endif
}

static void SmatchetDrawFrame(SmatchetUI& mainWindow, AppController& smatchetApp, PluginHost& pluginHost) {
    mainWindow.Draw(smatchetApp);
    pluginHost.OnDraw(smatchetApp);
}

static void SmatchetDrawFrameWithSeh(SmatchetUI& mainWindow, AppController& smatchetApp, PluginHost& pluginHost) {
#if defined(_WIN32) && defined(_MSC_VER)
    __try {
        SmatchetDrawFrame(mainWindow, smatchetApp, pluginHost);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "SEH exception in SmatchetUI::Draw — aborting frame.\n");
    }
#else
    SmatchetDrawFrame(mainWindow, smatchetApp, pluginHost);
#endif
}

}  // namespace

bool ParseStandaloneCli(int argc, char** argv, ConfigManager::CliOverrides& cli) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::fprintf(stdout,
                         "Smatchet Standalone Client\n"
                         "Usage: Smatchet [options]\n"
                         "Options:\n"
                         "  -d, --db-path <path>         Path to sqlite database\n"
                         "  -b, --backend-type <type>    Tracker type ('Jira' or 'Plane')\n"
                         "  -p, --mcp-port <port>        MCP server port number\n"
                         "      --mcp-allow-remote       Allow remote connections to MCP server\n"
                         "  -h, --help                   Show help message\n");
            return false;
        }
        if ((arg == "--db-path" || arg == "-d") && i + 1 < argc) {
            cli.HasDbPath = true;
            cli.DbPath = argv[++i];
            continue;
        }
        if ((arg == "--backend-type" || arg == "--tracker-type" || arg == "-b") && i + 1 < argc) {
            cli.HasBackendType = true;
            cli.BackendType = argv[++i];
            continue;
        }
        if ((arg == "--mcp-port" || arg == "-p") && i + 1 < argc) {
            try {
                cli.HasMcpPort = true;
                cli.McpPort = std::stoi(argv[++i]);
            } catch (...) {
            }
            continue;
        }
        if (arg == "--mcp-allow-remote") {
            cli.HasMcpAllowRemote = true;
            cli.McpAllowRemote = true;
            continue;
        }
    }
    return true;
}

bool InitAppAndPlugins(BootstrapContext& ctx, const TrackerConfig& cfg, const bool forceMcp, std::string& err) {
    err.clear();
    if (!ctx.window) {
        err = "InitAppAndPlugins requires an initialized GLFW window";
        return false;
    }

    try {
        if (!ctx.app) {
            ctx.app = new AppController();
            ctx.ownsApp = true;
        }
        if (!ctx.pluginHost) {
            ctx.pluginHost = new PluginHost();
            ctx.ownsPluginHost = true;
        }
        ctx.app->SetRuntimePluginHost(ctx.pluginHost);
        ctx.app->SetRequestAppQuitHandler([window = ctx.window]() {
            if (window) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        });

#if defined(SMATCHET_WITH_MCP)
        const bool wantMcp = cfg.McpEnabled || forceMcp;
        if (wantMcp) {
            const int mcpPort =
                (cfg.McpPort >= 1 && cfg.McpPort <= 65535) ? cfg.McpPort : SmatchetDefaults::Mcp::kDefaultPort;
            ctx.pluginHost->Register(std::make_unique<McpPlugin>(mcpPort));
        }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        ctx.pluginHost->Register(std::make_unique<LuaConsolePlugin>());
#endif
#if defined(SMATCHET_WITH_WHISPER)
        ctx.pluginHost->Register(std::make_unique<WhisperPlugin>());
#endif
        ctx.pluginHost->OnEarlyInit(*ctx.app);
        ctx.app->Initialize(cfg.DbPath, cfg.TrackerType);
        ctx.pluginHost->OnStart(*ctx.app);
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
    } catch (...) {
        err = "unknown exception during AppController boot";
    }
    return false;
}

bool Initialize(BootstrapContext& ctx, int argc, char** argv, HeadlessCliMode /*mode*/, std::string& err,
                const bool forceMcp) {
    (void)argc;
    (void)argv;
    err.clear();

    {
        const char* envUserData = std::getenv("SMATCHET_USER_DATA");
        if (envUserData && envUserData[0] != '\0') {
            const std::string userDataDir = SmatchetNormalizeDirectory(std::string(envUserData));
            SmatchetEnsureDirectoryExists(userDataDir);
            ConfigManager::SetUserDataDirectory(userDataDir);
        }
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        err = "glfwInit failed";
        return false;
    }

    SetupRuntimePaths();

    const TrackerConfig windowStateCfg = ConfigManager::Load();
    const int initialWindowW = std::max(320, windowStateCfg.WindowWidth);
    const int initialWindowH = std::max(240, windowStateCfg.WindowHeight);

#if defined(__APPLE__)
    ctx.glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    ctx.glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(initialWindowW, initialWindowH, "Smatchet - Standalone", NULL, NULL);
    if (window == NULL) {
        err = "glfwCreateWindow failed";
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string s_imguiIniPath = ConfigManager::GetImGuiSettingsPath();
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    glfwSetKeyCallback(window, SmatchetKeypadEnterBridgeCallback);
    ImGui_ImplOpenGL3_Init(ctx.glslVersion);
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        err = "ImGui OpenGL device objects failed";
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return false;
    }

    ConfigManager::CliOverrides cli;
    if (!ParseStandaloneCli(argc, argv, cli)) {
        err = "help";
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load(cli);

    ctx.window = window;
    if (InitAppAndPlugins(ctx, cfg, forceMcp, err)) {
        return true;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return false;
}

void RunRenderLoop(BootstrapContext& ctx, const std::function<bool()>& shouldStop) {
    if (!ctx.window || !ctx.app || !ctx.pluginHost) {
        return;
    }

    if (!ctx.mainWindow) {
        ctx.mainWindow = new SmatchetUI();
        ctx.ownsMainWindow = true;
    }

    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();
        ctx.app->mainThreadDispatcher.Drain();

        bool scenScrollActive = false;
        int scenScrollTarget = -1;
        ctx.app->Scenarios().Tick(*ctx.app, scenScrollActive, scenScrollTarget);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_NoUndocking);
        SmatchetDrawFrameWithSeh(*ctx.mainWindow, *ctx.app, *ctx.pluginHost);
        ImGui::Render();

        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(ctx.window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        glClearColor(bg.x * bg.w, bg.y * bg.w, bg.z * bg.w, bg.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(ctx.window);

        if (shouldStop && shouldStop()) {
            break;
        }
    }
}

void ShutdownApplication(BootstrapContext& ctx) {
    if (ctx.app && ctx.pluginHost) {
        ctx.app->ClearAutomationLogSinks();
        ctx.app->SetRuntimePluginHost(nullptr);
        ctx.pluginHost->OnStop();
    }

    if (ctx.ownsMainWindow && ctx.mainWindow) {
        delete ctx.mainWindow;
        ctx.mainWindow = nullptr;
        ctx.ownsMainWindow = false;
    }
    if (ctx.ownsApp && ctx.app) {
        delete ctx.app;
        ctx.app = nullptr;
        ctx.ownsApp = false;
    }
    if (ctx.ownsPluginHost && ctx.pluginHost) {
        delete ctx.pluginHost;
        ctx.pluginHost = nullptr;
        ctx.ownsPluginHost = false;
    }
}

bool BootEphemeral(int argc, char** argv, std::string& err) {
    BootstrapContext ctx;
    ConfigManager::CliOverrides cli;
    if (!ParseStandaloneCli(argc, argv, cli)) {
        err.clear();
        return true;
    }
    if (!Initialize(ctx, argc, argv, HeadlessCliMode::ScenarioRun, err, true)) {
        if (err == "help") {
            err.clear();
            return true;
        }
        return false;
    }
    RunRenderLoop(ctx, [&ctx]() { return glfwWindowShouldClose(ctx.window) != 0; });
    Shutdown(ctx);
    err.clear();
    return true;
}

void Shutdown(BootstrapContext& ctx) {
    ShutdownApplication(ctx);

    if (ctx.window) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(ctx.window);
        ctx.window = nullptr;
        glfwTerminate();
    }
}

}  // namespace standalone
}  // namespace smatchet
