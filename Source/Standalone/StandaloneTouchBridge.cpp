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

// The router to feed this finger event, or null when the event is not from a touchscreen or touch
// gestures are switched off (then the backend handles it exactly as before).
TouchInputRouter* RouterForTouchEvent() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr || ctx->InputEventsNextMouseSource != ImGuiMouseSource_TouchScreen) {
        return nullptr;
    }
    TouchInputRouter* router = TouchInputRouter::ForContext(ctx);
    return (router != nullptr && router->GesturesEnabled()) ? router : nullptr;
}

void TouchBridgeCursorPos(GLFWwindow* window, double x, double y) {
    if (TouchInputRouter* router = RouterForTouchEvent()) {
        router->OnRawMove(static_cast<float>(x), static_cast<float>(y));
        return;
    }
    ImGui_ImplGlfw_CursorPosCallback(window, x, y);
}

void TouchBridgeMouseButton(GLFWwindow* window, int button, int action, int mods) {
    if (TouchInputRouter* router = RouterForTouchEvent()) {
        router->OnRawButton(button, action == GLFW_PRESS);
        return;
    }
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
}

void TouchBridgeCursorEnter(GLFWwindow* window, int entered) {
    // After a tap the pointer stays where the finger was so hover tooltips can show; a touch
    // cursor-leave would move it off-window (-FLT_MAX) and close them.
    if (!entered) {
        TouchInputRouter* router = RouterForTouchEvent();
        if (router != nullptr && router->IsParked()) {
            return;
        }
    }
    ImGui_ImplGlfw_CursorEnterCallback(window, entered);
}

// Windows turns a held finger into its own right click (after a press-and-hold animation). The router
// already opens the context menu on a hold, so switch the OS gesture off while touch gestures are on
// and give it back when they are switched Off.
void SyncTabletPressAndHold(GLFWwindow* window, bool gesturesEnabled) {
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
    if (gesturesEnabled) {
        ::SetPropW(hwnd, kProperty, reinterpret_cast<HANDLE>(kTabletDisablePressAndHold | kTabletDisableFlicks));
    } else {
        ::RemovePropW(hwnd, kProperty);
    }
    if (atom != 0) {
        ::GlobalDeleteAtom(atom);
    }
#else
    (void)window;
    (void)gesturesEnabled;
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
        SyncTabletPressAndHold(window, router->GesturesEnabled());
        router->SetGesturesEnabledChangedCallback([window](bool enabled) { SyncTabletPressAndHold(window, enabled); });
    }
}

} // namespace standalone
} // namespace smatchet
