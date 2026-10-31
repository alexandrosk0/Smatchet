#include "StandaloneTouchBridge.h"

#include "StandaloneBoot_detail.h"
#include "Ui/TouchInputRouter.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_internal.h" // ImGuiContext::InputEventsNextMouseSource (the backend's source latch)

#include <GLFW/glfw3.h>

#if defined(_WIN32)
#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>
#endif

namespace smatchet {
namespace standalone {

namespace {

using smatchet::ui::touch::TouchInputRouter;

void SyncTabletPressAndHold(GLFWwindow* window, bool disableOsGesture);

// Windows' own press-and-hold right click is switched off only once a touchscreen has actually been
// used (and while gestures are on). The same window property also governs the PEN, which the router
// does not handle, so setting it up front would take hold-to-right-click away from pen users on a
// machine that never sees a finger.
bool g_touchSeen = false;
bool g_gesturesEnabled = true;

void UpdateTabletPressAndHold(GLFWwindow* window) { SyncTabletPressAndHold(window, g_touchSeen && g_gesturesEnabled); }

// The router to feed this finger event, or null when the event is not from a touchscreen or touch
// gestures are switched off (then the backend handles it exactly as before).
TouchInputRouter* RouterForTouchEvent(GLFWwindow* window) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr || ctx->InputEventsNextMouseSource != ImGuiMouseSource_TouchScreen) {
        return nullptr;
    }
    if (!g_touchSeen) {
        g_touchSeen = true;
        UpdateTabletPressAndHold(window);
    }
    TouchInputRouter* router = TouchInputRouter::ForContext(ctx);
    return (router != nullptr && router->GesturesEnabled()) ? router : nullptr;
}

void TouchBridgeCursorPos(GLFWwindow* window, double x, double y) {
    if (TouchInputRouter* router = RouterForTouchEvent(window)) {
        router->OnRawMove(static_cast<float>(x), static_cast<float>(y));
        return;
    }
    ImGui_ImplGlfw_CursorPosCallback(window, x, y);
}

void TouchBridgeMouseButton(GLFWwindow* window, int button, int action, int mods) {
    if (TouchInputRouter* router = RouterForTouchEvent(window)) {
        router->OnRawButton(button, action == GLFW_PRESS);
        return;
    }
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
}

void TouchBridgeCursorEnter(GLFWwindow* window, int entered) {
    // After a tap the pointer stays where the finger was so hover tooltips can show; a touch
    // cursor-leave would move it off-window (-FLT_MAX) and close them.
    if (!entered) {
        TouchInputRouter* router = RouterForTouchEvent(window);
        if (router != nullptr && router->IsParked()) {
            return;
        }
    }
    ImGui_ImplGlfw_CursorEnterCallback(window, entered);
}

// Windows turns a held finger into its own right click (after a press-and-hold animation). The router
// already opens the context menu on a hold, so the OS gesture is switched off while the router is
// handling a touchscreen, and given back when touch gestures are switched Off.
void SyncTabletPressAndHold(GLFWwindow* window, bool disableOsGesture) {
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr) {
        return;
    }
    // <tpcshrd.h> values, spelled out to avoid pulling the Tablet PC header in.
    const DWORD_PTR kTabletDisablePressAndHold = 0x00000001;
    const DWORD_PTR kTabletDisableFlicks = 0x00010000;
    const wchar_t* const kProperty = L"MicrosoftTabletPenServiceProperty";
    const ATOM atom = ::GlobalAddAtomW(kProperty);
    if (disableOsGesture) {
        ::SetPropW(hwnd, kProperty, reinterpret_cast<HANDLE>(kTabletDisablePressAndHold | kTabletDisableFlicks));
    } else {
        ::RemovePropW(hwnd, kProperty);
    }
    if (atom != 0) {
        ::GlobalDeleteAtom(atom);
    }
#else
    (void)window;
    (void)disableOsGesture;
#endif
}

} // namespace

void InstallStandaloneInputBridges(GLFWwindow* window) {
    glfwSetKeyCallback(window, boot_detail::KeypadEnterBridgeCallback);
    glfwSetCursorPosCallback(window, TouchBridgeCursorPos);
    glfwSetMouseButtonCallback(window, TouchBridgeMouseButton);
    glfwSetCursorEnterCallback(window, TouchBridgeCursorEnter);

    TouchInputRouter* router = TouchInputRouter::ForContext(ImGui::GetCurrentContext());
    if (router != nullptr) {
        g_gesturesEnabled = router->GesturesEnabled();
        router->SetGesturesEnabledChangedCallback([window](bool enabled) {
            g_gesturesEnabled = enabled;
            UpdateTabletPressAndHold(window);
        });
    }
}

void UninstallStandaloneInputBridges(GLFWwindow* window) {
    TouchInputRouter* router = TouchInputRouter::ForContext(ImGui::GetCurrentContext());
    if (router != nullptr) {
        router->SetGesturesEnabledChangedCallback(nullptr);
    }
    SyncTabletPressAndHold(window, false); // Win32: remove window properties before the window goes
    g_touchSeen = false;
}

} // namespace standalone
} // namespace smatchet
