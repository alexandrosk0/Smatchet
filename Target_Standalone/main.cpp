#include <memory>
#include <string>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <vector>

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

#if defined(_WIN32)
// winsock2.h must be included before windows.h (including indirectly via other
// headers) to avoid MinGW's "-Wcpp" warning.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
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

int main(int argc, char** argv) {
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

    // Set config/views/Lua scripts base path to exe directory (after window/context so startup is stable).
#if defined(_WIN32)
    {
        char buf[MAX_PATH];
        if (GetModuleFileNameA(NULL, buf, MAX_PATH)) {
            std::string exePath(buf);
            std::string::size_type last = exePath.find_last_of("\\/");
            if (last != std::string::npos) {
                std::string exeDir = exePath.substr(0, last + 1);
                ConfigManager::SetBaseDirectoryForFiles(exeDir);
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
                ConfigManager::SetBaseDirectoryForFiles(exePath.substr(0, last + 1));
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
                ConfigManager::SetBaseDirectoryForFiles(exePath.substr(0, last + 1));
            }
        }
    }
#endif

    // 2. Setup Dear ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string s_imguiIniPath = ConfigManager::GetImGuiSettingsPath();
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
#if defined(SMATCHET_WITH_MCP)
        {
            if (cfg.McpEnabled) {
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

            // Setup a full-screen dockspace for a professional layout
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            // Draw the main application
            SmatchetDrawFrameWithSeh(mainWindow, smatchetApp, pluginHost);

            // Rendering
            ImGui::Render();
            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w,
                         clear_color.w);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);

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





