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

#include "StandaloneBoot_detail.h"

#include "AppController.h"
#include "Dx12Bootstrap.h"
#include "GridContextDepsAdapter.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "ITrackerBackendFactory.h"
#include "LuaAutomationHost.h"
#include "OfflineQueueService.h"
#include "PluginHost.h"
#include "TicketSyncService.h"
#include "Commands/Scenarios/IScenario.h"
#include "SmatchetDefaults.h"
#include "Ui/P4ClPreview.h"
#include "Ui/SmatchetImGuiTextureGuardRuntime.h"
#include "Ui/SmatchetTooltipWheelRouter.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetUI.h"
#include "SmatchetUiSession.h"
#include "VsyncControl.h"

#include "Logger.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

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

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h> // glfwGetWin32Window — HWND for the DX12 swapchain
#endif

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

#if defined(SMATCHET_BUILD_UI_TESTS)
// Fixture-backed backend injection for deterministic Jira UI tests.
// Headers live in tests/support/ — added to the include path by tests/ui/CMakeLists.txt.
#include "Commands/Scenarios/UiTestScenario.h"
#include "JiraFakeTrackerFixture.h"
#include "ScriptedTrackerBackendFactory.h"
#include "imgui_te_engine.h"
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
BootstrapContext::BootstrapContext() = default;
BootstrapContext::~BootstrapContext() = default;
} // namespace standalone
} // namespace smatchet

namespace smatchet {
namespace standalone {

namespace {

// Path / callback / pixel leaf helpers are shared with main.cpp via
// StandaloneBoot_detail (de-dup of the dup_audit `duplication` WARN). Local
// aliases keep the existing call sites readable + behaviour byte-identical.
using boot_detail::EnsureDirectoryExists;
using boot_detail::KeypadEnterBridgeCallback;
using boot_detail::NormalizeDirectory;
using boot_detail::PackRgbDropAlpha;
using boot_detail::ResolveStandaloneUserDataDir;

static void glfw_error_callback(int error, const char* description) {
    LOG_ERROR("GLFW Error %d: %s", error, description);
}

static void SetupRuntimePaths(const bool preserveUserDataDir) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetModuleFileNameA(NULL, buf, MAX_PATH)) {
        std::string exePath(buf);
        const std::string::size_type last = exePath.find_last_of("\\/");
        if (last != std::string::npos) {
            const std::string exeDir = NormalizeDirectory(exePath.substr(0, last + 1));
            ConfigManager::SetRuntimeAssetDirectory(exeDir);
            if (!preserveUserDataDir) {
                const std::string userDataDir = ResolveStandaloneUserDataDir(exeDir);
                EnsureDirectoryExists(userDataDir);
                ConfigManager::SetUserDataDirectory(userDataDir);
            }
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
            const std::string exeDir = NormalizeDirectory(exePath.substr(0, last + 1));
            ConfigManager::SetRuntimeAssetDirectory(exeDir);
            if (!preserveUserDataDir) {
                const std::string userDataDir = ResolveStandaloneUserDataDir(exeDir);
                EnsureDirectoryExists(userDataDir);
                ConfigManager::SetUserDataDirectory(userDataDir);
            }
        }
    }
#elif defined(__APPLE__)
    std::vector<char> buf(PATH_MAX);
    uint32_t sz = static_cast<uint32_t>(buf.size());
    if (_NSGetExecutablePath(buf.data(), &sz) == 0) {
        std::string exePath(buf.data());
        const std::string::size_type last = exePath.find_last_of('/');
        if (last != std::string::npos) {
            const std::string exeDir = NormalizeDirectory(exePath.substr(0, last + 1));
            ConfigManager::SetRuntimeAssetDirectory(exeDir);
            if (!preserveUserDataDir) {
                const std::string userDataDir = ResolveStandaloneUserDataDir(exeDir);
                EnsureDirectoryExists(userDataDir);
                ConfigManager::SetUserDataDirectory(userDataDir);
            }
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
        LOG_ERROR("SEH exception in SmatchetUI::Draw; terminating process.");
        std::terminate();
    }
#else
    SmatchetDrawFrame(mainWindow, smatchetApp, pluginHost);
#endif
}

// PackRgbDropAlpha lives in StandaloneBoot_detail (shared with main.cpp).

// Read the just-presented frame into top-left-origin RGB. DX12 reads its swapchain
// back buffer (already top-left, no flip); GL reads the front buffer (bottom-left,
// flipped here). Returns false on an empty framebuffer / capture failure.
static bool CaptureFrameRgb(BootstrapContext& ctx, std::vector<unsigned char>& rgb, int& outW, int& outH) {
    if (ctx.renderer == StandaloneRenderer::Dx12) {
        std::vector<uint8_t> rgba;
        int w = 0;
        int h = 0;
        if (!ctx.dx12 || !ctx.dx12->CaptureBackbuffer(rgba, w, h) || w <= 0 || h <= 0) {
            return false;
        }
        PackRgbDropAlpha(rgba.data(), w, h, /*flipVertical=*/false, rgb);
        outW = w;
        outH = h;
        return true;
    }

    int fw = 0;
    int fh = 0;
    glfwGetFramebufferSize(ctx.window, &fw, &fh);
    if (fw <= 0 || fh <= 0) {
        return false;
    }
    std::vector<unsigned char> pixels(static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4u);
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    PackRgbDropAlpha(pixels.data(), fw, fh, /*flipVertical=*/true, rgb);
    outW = fw;
    outH = fh;
    return true;
}

static void SmatchetWritePendingScreenshot(BootstrapContext& ctx) {
    if (!g_ui.requestScreenshot) {
        return;
    }

    g_ui.requestScreenshot = false;
    const std::string screenshotPath = g_ui.requestScreenshotPath;
    int fw = 0;
    int fh = 0;
    std::vector<unsigned char> rgb;
    if (!CaptureFrameRgb(ctx, rgb, fw, fh) || fw <= 0 || fh <= 0 || screenshotPath.empty()) {
        LOG_ERROR("debug.window.screenshot: invalid framebuffer or empty path");
        return;
    }

    stbi_write_png_compression_level = 8;
    const int rc = stbi_write_png(screenshotPath.c_str(), fw, fh, 3, rgb.data(), fw * 3);
    if (rc == 0) {
        LOG_ERROR("debug.window.screenshot: stbi_write_png failed for %s", screenshotPath.c_str());
    } else {
        LOG_INFO("debug.window.screenshot: saved %s (%dx%d, PNG)", screenshotPath.c_str(), fw, fh);
    }
}

static void SmatchetApplyPendingWindowResize(GLFWwindow* window) {
    if (!g_ui.requestWindowResize) {
        return;
    }

    g_ui.requestWindowResize = false;
    const int w = std::max(320, g_ui.requestWindowWidth);
    const int h = std::max(240, g_ui.requestWindowHeight);
    glfwSetWindowSize(window, w, h);
    LOG_INFO("debug.window.resize: %dx%d", w, h);
}

// Honour SMATCHET_USER_DATA: normalize + ensure the dir exists + point ConfigManager at it.
void ApplyUserDataEnvOverride() {
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

// Per-platform GLFW window hints; sets ctx.glslVersion. Window starts hidden.
// DX12 takes a GLFW_NO_API window (no GL context — it drives its own swapchain).
void ConfigureGlfwWindowHints(BootstrapContext& ctx) {
    if (ctx.renderer == StandaloneRenderer::Dx12) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        ctx.glslVersion = nullptr;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        return;
    }
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
}

// Set up the ImGui context: ini path (migrating the layout schema when stale), config flags,
// dark theme, and the extended-glyph default font.
void SetupImGuiContext() {
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
}

} // namespace

bool ParseStandaloneCli(int argc, char** argv, ConfigManager::CliOverrides& cli) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::fprintf(stdout,
                         "Smatchet Standalone Client\n" // CLI stdout — product output, not logging
                         "Usage: Smatchet [options]\n"
                         "Options:\n"
                         "  -d, --db-path <path>         Path to sqlite database\n"
                         "  -b, --backend-type <type>    Tracker type ('Jira' or 'Plane')\n"
                         "  -p, --mcp-port <port>        MCP server port number\n"
                         "      --mcp-allow-remote       Allow remote connections to MCP server\n"
                         "      --vsync | --no-vsync     Force vsync on/off for this launch (not persisted)\n"
                         "      --renderer <dx12|gl>     Force renderer (default: dx12 on Windows, gl elsewhere)\n"
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
                cli.McpPort = std::stoi(argv[++i]);
                cli.HasMcpPort = true;
            } catch (...) { // catch-all-ok: invalid --mcp-port -> configured/default port (pre-logger-init)
                // Invalid --mcp-port value — ignore the flag and fall back to the
                // configured/default port. Pre-logger-init, so no LOG_* here.
                cli.HasMcpPort = false;
            }
            continue;
        }
        if (arg == "--mcp-allow-remote") {
            cli.HasMcpAllowRemote = true;
            cli.McpAllowRemote = true;
            continue;
        }
        if (arg == "--vsync") {
            cli.HasVsync = true;
            cli.Vsync = true;
            continue;
        }
        if (arg == "--no-vsync") {
            cli.HasVsync = true;
            cli.Vsync = false;
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
            ctx.app = std::make_unique<AppController>();
        }
        if (!ctx.pluginHost) {
            ctx.pluginHost = std::make_unique<PluginHost>();
        }
        ctx.app->SetRuntimePluginHost(ctx.pluginHost.get());
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
#else
        (void)forceMcp; // MCP compiled out — forceMcp has no consumer in this build.
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        ctx.pluginHost->Register(std::make_unique<LuaConsolePlugin>());
#endif
#if defined(SMATCHET_WITH_WHISPER)
        ctx.pluginHost->Register(std::make_unique<WhisperPlugin>());
#endif
        ctx.pluginHost->OnEarlyInit(*ctx.app);
#if defined(SMATCHET_BUILD_UI_TESTS)
        {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
            const char* fixturePath = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
            if (fixturePath != nullptr && fixturePath[0] != '\0') {
                ctx.app->SetBackendFactory(std::make_unique<smatchet_tests::ScriptedTrackerBackendFactory>(
                    smatchet_tests::JiraFakeTrackerFixture::LoadFromFile(fixturePath)));
            }
        }
#endif
        ctx.app->Initialize(cfg.DbPath, cfg.TrackerType);
        ctx.pluginHost->OnStart(*ctx.app);
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
    } catch (...) { // catch-all-ok: boot failure is returned to caller as err.
        err = "unknown exception during AppController boot";
    }
    return false;
}

// Tear down a partially-initialised standalone boot — renderer backend (DX12
// swapchain or GL3 device objects), the ImGui platform backend + context, and
// the GLFW window. Shared by Initialize's three failure exits so the unwind
// order stays identical on every path. Null-safe: a DX12 init that failed has
// already reset ctx.dx12, so the swapchain branch is skipped.
//
// CPP_CODE_AUDIT.md #29: `glfwBackendInitialized`/`rendererBackendInitialized`
// default to true (the "everything succeeded, we're just unwinding a LATER
// failure" case — e.g. ParseStandaloneCli failing after InitRendererBackend
// already returned true). InitRendererBackend's own failure exit passes the
// actual per-step outcome, since a Shutdown() call with no matching Init() is
// a null-deref once IM_ASSERT is compiled out in release.
void TeardownPartialBoot(BootstrapContext& ctx, GLFWwindow* window, const bool useDx12,
                         const bool glfwBackendInitialized = true, const bool rendererBackendInitialized = true) {
    if (useDx12) {
        if (ctx.dx12) {
            ctx.dx12->Shutdown();
        }
    } else if (rendererBackendInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
    }
    if (glfwBackendInitialized) {
        ImGui_ImplGlfw_Shutdown();
    }
    ImGui::DestroyContext();
    if (window != NULL) {
        glfwDestroyWindow(window);
    }
    glfwTerminate();
}

// Initialise the ImGui platform + renderer backend for ctx.renderer. On success
// the chosen backend is live (DX12 swapchain ready, or GL3 device objects
// created); on failure `err` is set and the partial DX12 state is rolled back —
// the caller still owns the window/GLFW unwind via TeardownPartialBoot.
// `outGlfwBackendInitialized`/`outRendererBackendInitialized` report exactly which
// steps succeeded before the failure, so a caller unwinding via TeardownPartialBoot
// only Shutdown()s backends that were actually Init()'d (CPP_CODE_AUDIT.md #29).
bool InitRendererBackend(BootstrapContext& ctx, GLFWwindow* window, std::string& err, bool& outGlfwBackendInitialized,
                         bool& outRendererBackendInitialized) {
    outGlfwBackendInitialized = false;
    outRendererBackendInitialized = false;
    if (ctx.renderer == StandaloneRenderer::Dx12) {
        if (!ImGui_ImplGlfw_InitForOther(window, true)) {
            err = "ImGui GLFW platform backend init failed (DX12 path)";
            return false;
        }
        outGlfwBackendInitialized = true;
        glfwSetKeyCallback(window, KeypadEnterBridgeCallback);
        int fbW = 0;
        int fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);
#if defined(_WIN32)
        void* hwnd = glfwGetWin32Window(window);
#else
        void* hwnd = nullptr;
#endif
        ctx.dx12 = std::make_unique<Dx12Bootstrap>();
        std::string dxErr;
        if (!ctx.dx12->Init(hwnd, fbW, fbH, dxErr)) {
            err = "DX12 renderer init failed: " + dxErr;
            ctx.dx12.reset();
            return false;
        }
        LOG_INFO("DX12 renderer initialised (adapter: %s%s).", ctx.dx12->AdapterDescription().c_str(),
                 ctx.dx12->IsWarp() ? ", WARP software" : "");
        return true;
    }
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        err = "ImGui GLFW platform backend init failed (OpenGL path)";
        return false;
    }
    outGlfwBackendInitialized = true;
    glfwSetKeyCallback(window, KeypadEnterBridgeCallback);
    if (!ImGui_ImplOpenGL3_Init(ctx.glslVersion)) {
        err = "ImGui OpenGL3 renderer backend init failed";
        return false;
    }
    outRendererBackendInitialized = true;
    if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
        err = "ImGui OpenGL device objects failed";
        return false;
    }
    return true;
}

bool Initialize(BootstrapContext& ctx, int argc, char** argv, HeadlessCliMode /*mode*/, std::string& err,
                const bool forceMcp) {
    err.clear();

    // Resolve the renderer before any window hint is set — the hint choice (GL
    // context vs GLFW_NO_API) and the backend-init branch below both depend on it.
    ctx.renderer = ResolveStandaloneRenderer(argc, argv);
    const bool useDx12 = (ctx.renderer == StandaloneRenderer::Dx12);

    ApplyUserDataEnvOverride();

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        err = "glfwInit failed";
        return false;
    }

    SetupRuntimePaths(!ConfigManager::GetUserDataDirectory().empty());

    const TrackerConfig windowStateCfg = ConfigManager::Load();
    int initialWindowW = std::max(320, windowStateCfg.WindowWidth);
    int initialWindowH = std::max(240, windowStateCfg.WindowHeight);
#if defined(SMATCHET_BUILD_UI_TESTS)
    // Bucket-E spawns this GUI hidden with an empty user-data dir, so the window falls to
    // the 320 px floor above. UiMode::Auto (the default) resolves that narrow width to
    // Mobile, which draws the mobile shell and occludes all desktop chrome — every ui-test
    // asserting a desktop window then fails. Pin a deterministic desktop framebuffer so
    // Auto resolves Desktop (the mode every pre-mobile ui-test was written against).
    // Mobile-shell tests opt back in by pinning cfg.UiMode = UiMode::Mobile (ignores width).
    initialWindowW = std::max(initialWindowW, 1280);
    initialWindowH = std::max(initialWindowH, 800);
#endif

    ConfigureGlfwWindowHints(ctx);

    GLFWwindow* window = glfwCreateWindow(initialWindowW, initialWindowH, "Smatchet - Standalone", NULL, NULL);
    if (window == NULL) {
        err = "glfwCreateWindow failed";
        glfwTerminate();
        return false;
    }

    if (!useDx12) {
        glfwMakeContextCurrent(window);
        // Initial vsync-on; the resolved value (env > --vsync/--no-vsync flag >
        // config) is applied right after the ParseStandaloneCli call below — this
        // path parses the standalone CLI itself, so the flag works here too. DX12
        // has no GL context and applies vsync per-frame in Present(syncInterval).
        glfwSwapInterval(1);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    SetupImGuiContext();

    bool glfwBackendInitialized = false;
    bool rendererBackendInitialized = false;
    if (!InitRendererBackend(ctx, window, err, glfwBackendInitialized, rendererBackendInitialized)) {
        TeardownPartialBoot(ctx, window, useDx12, glfwBackendInitialized, rendererBackendInitialized);
        return false;
    }

    ConfigManager::CliOverrides cli;
    if (!ParseStandaloneCli(argc, argv, cli)) {
        err = "help";
        TeardownPartialBoot(ctx, window, useDx12);
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load(cli);

    // Seed the live vsync hub with the documented precedence and apply it to the
    // (current) hidden-window GL context — --vsync/--no-vsync works on this path
    // too since the parse above consumes the same standalone grammar (CR-953).
    // DX12 carries the resolved value per-frame into Present (no GL context here).
    {
        const char* vsyncSource = smatchet::vsync::SeedFromBootSources(cfg.VsyncEnabled, cli.HasVsync, cli.Vsync);
        LOG_INFO("Vsync %s at boot (source: %s)", smatchet::vsync::Enabled() ? "enabled" : "disabled", vsyncSource);
        if (!useDx12) {
            glfwSwapInterval(smatchet::vsync::Enabled() ? 1 : 0);
        }
    }

    ctx.window = window;
    // Latch the ephemeral-spawn identity for the whole session (see
    // UiDrawSession::ephemeralSession). forceMcp is set exactly by `--ephemeral`.
    g_ui.ephemeralSession = forceMcp;
    if (InitAppAndPlugins(ctx, cfg, forceMcp, err)) {
        return true;
    }

    TeardownPartialBoot(ctx, window, useDx12);
    return false;
}

void RunRenderLoop(BootstrapContext& ctx, const std::function<bool()>& shouldStop) {
    if (!ctx.window || !ctx.app || !ctx.pluginHost) {
        return;
    }

    if (!ctx.mainWindow) {
        ctx.mainWindow = std::make_unique<SmatchetUI>();
    }

    const bool useDx12 = (ctx.renderer == StandaloneRenderer::Dx12);

    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();
        // Gated on cfgInitialized: until SmatchetUI::drawInitConfigOnce has run,
        // g_ui.cfg holds default-constructed values that its wholesale
        // `g_ui.cfg = ConfigManager::Load()` is about to overwrite. Draining
        // before that latch lets a command handler write g_ui.cfg only to have
        // the load clobber it back to the on-disk values — which is how the
        // bucket-C user-info-* scenarios intermittently captured the whisper
        // first-run banner despite setting WhisperSetupCompleted in OnStart.
        // Skipping the drain for the pre-first-Draw frames only defers the
        // command by one frame; the in-Draw drain still runs every frame.
        if (g_ui.cfgInitialized) {
            ctx.app->mainThreadDispatcher.Drain();
        }

        if (useDx12) {
            ctx.dx12->NewFrame();
        } else {
            ImGui_ImplOpenGL3_NewFrame();
        }
        ImGui_ImplGlfw_NewFrame();
        smatchet::ui::RouteWheelToScrollableTooltipBeforeNewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);

        SmatchetDrawFrameWithSeh(*ctx.mainWindow, *ctx.app, *ctx.pluginHost);

        // Scenarios().Tick MUST run INSIDE the NewFrame/Render bracket and after
        // the app draw, matching SmatchetUI::Draw's production ordering. Scenario
        // OnFrame may touch per-frame ImGui state (GetForegroundDrawList,
        // GetWindowDrawList), and ui_test.run completion must observe the frame
        // after test GuiFunc submission rather than racing ahead of it.
        bool scenScrollActive = false;
        int scenScrollTarget = -1;
        ctx.app->Scenarios().Tick(*ctx.app, scenScrollActive, scenScrollTarget);

        ImGui::Render();

        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(ctx.window, &display_w, &display_h);
        const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        // [P0 #1122] shared dynamic-texture guard — MUST run here too: this ephemeral
        // (--spawn / headless scenario) render loop is a separate loop from main.cpp's
        // RunFrameLoop, and the mobile-texture-guard scenario forces its faults inside it.
        ImDrawData* drawData = ImGui::GetDrawData();
        smatchet::ui::GuardImGuiDynamicTextures(drawData);
        if (useDx12) {
            ctx.dx12->Resize(display_w, display_h);
            if (display_w > 0 && display_h > 0) {
                ctx.dx12->BeginFrame(bg); // premultiplied clear inside, matching the GL path
                ctx.dx12->RenderDrawData(drawData);
                ctx.dx12->Present(smatchet::vsync::Enabled() ? 1 : 0);
            }
        } else {
            glViewport(0, 0, display_w, display_h);
            glClearColor(bg.x * bg.w, bg.y * bg.w, bg.z * bg.w, bg.w);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(drawData);
            glfwSwapBuffers(ctx.window);
        }
        SmatchetApplyPendingWindowResize(ctx.window);
        SmatchetWritePendingScreenshot(ctx);

#if defined(SMATCHET_BUILD_UI_TESTS)
        if (ImGuiTestEngine* uiTestEngine = SmatchetActiveUiTestEngine()) {
            ImGuiTestEngine_PostSwap(uiTestEngine);
        }
#endif

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

    // CPP_CODE_AUDIT.md #30: bounded-timeout drain before the mainWindow/app/pluginHost
    // reset below runs any remaining teardown — see P4ClPreview::DrainForShutdown's doc
    // comment for why this can't just be left to the Meyers singleton's own destructor.
    P4ClPreview::DrainForShutdown();

    ctx.mainWindow.reset();
    ctx.app.reset();
    ctx.pluginHost.reset();
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
        if (ctx.dx12) {
            ctx.dx12->Shutdown();
            ctx.dx12.reset();
        } else {
            ImGui_ImplOpenGL3_Shutdown();
        }
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(ctx.window);
        ctx.window = nullptr;
        glfwTerminate();
    }
}

} // namespace standalone
} // namespace smatchet
