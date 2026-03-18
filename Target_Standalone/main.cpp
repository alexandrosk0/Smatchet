#include <string>
#include <exception>
#include <cstdio>

// ImGui Core and Backends
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h> // Will drag in system OpenGL headers

// Smatchet Core Headers (Safe C++14 includes)
#include "ConfigManager.h"
#include "AppController.h"
#include "SmatchetUI.h"
#include "McpServer.h"

// GLFW Error Callback
static void glfw_error_callback(int error, const char* description) {
    ::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int, char**) {
    // 1. Setup OS Window (GLFW)
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

    // Decide GL+GLSL versions
#if defined(_WIN32)
#include <windows.h>
#endif

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

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Smatchet - Standalone", NULL, NULL);
    if (window == NULL) {
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Set config/views base path to exe directory (after window/context so startup is stable).
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
#endif

    // 2. Setup Dear ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking

    // Setup ImGui Style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // 3. Initialize Smatchet Core
    AppController smatchetApp;
    SmatchetUI mainWindow;

    // Boot up the database, plugins, and prepare the Jira backend
    smatchetApp.Initialize("Smatchet_LocalCache.sqlite", "Jira");

    McpServer mcp(smatchetApp);
    mcp.Start(8080);
    
    // 4. The Main Render Loop
    ImVec4 clear_color = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

    while (!glfwWindowShouldClose(window)) {
        // Poll and handle events (inputs, window resize, etc.)
        glfwPollEvents();

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
#if defined(_WIN32) && defined(_MSC_VER)
        __try {
#endif
        try {
            mainWindow.Draw(smatchetApp);
        } catch (const std::exception&) {
            throw;
        } catch (...) {
            throw;
        }
#if defined(_WIN32) && defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exit(1);
        }
#endif

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // 5. Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
