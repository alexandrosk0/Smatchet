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

// Windows' own press-and-hold right click is switched off only while a FINGER is the active pointer
// (and gestures are on). The same window property also governs the PEN, which the router does not
// handle, so it follows the device in use: set by touch input, cleared by pen input (a pen hovers
// before it touches down, so the property is gone by the time a pen hold starts). Mouse input leaves
// it as it is.
bool g_touchActive = false;
bool g_gesturesEnabled = true;
bool g_propertySet = false;

void UpdateTabletPressAndHold(GLFWwindow* window) {
    const bool want = g_touchActive && g_gesturesEnabled;
    if (want != g_propertySet) {
        SyncTabletPressAndHold(window, want);
        g_propertySet = want;
    }
}

// Track which device the current Windows mouse message came from (the backend's WndProc latch).
void NoteSource(GLFWwindow* window, ImGuiMouseSource source) {
    if (source == ImGuiMouseSource_TouchScreen || source == ImGuiMouseSource_Pen) {
        g_touchActive = (source == ImGuiMouseSource_TouchScreen);
        UpdateTabletPressAndHold(window);
    }
}

// The router to feed this finger event, or null when the event is not from a touchscreen or touch
// gestures are switched off (then the backend handles it exactly as before).
TouchInputRouter* RouterForTouchEvent(GLFWwindow* window) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr) {
        return nullptr;
    }
    NoteSource(window, ctx->InputEventsNextMouseSource);
    if (ctx->InputEventsNextMouseSource != ImGuiMouseSource_TouchScreen) {
        return nullptr;
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

// Where the pointer was when the button message being handled was posted, in GLFW client
// coordinates. On Windows that is GetMessagePos (glfwGetCursorPos would read the cursor NOW, which
// can be ahead of a press still being processed); elsewhere fall back to the current cursor.
void PressPointInClient(GLFWwindow* window, float& x, float& y) {
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd != nullptr) {
        const DWORD pos = ::GetMessagePos();
        POINT pt;
        pt.x = static_cast<short>(LOWORD(pos)); // signed: multi-monitor screens can be negative
        pt.y = static_cast<short>(HIWORD(pos));
        if (::ScreenToClient(hwnd, &pt)) {
            x = static_cast<float>(pt.x);
            y = static_cast<float>(pt.y);
            return;
        }
    }
#endif
    double cx = 0.0;
    double cy = 0.0;
    glfwGetCursorPos(window, &cx, &cy);
    x = static_cast<float>(cx);
    y = static_cast<float>(cy);
}

void TouchBridgeMouseButton(GLFWwindow* window, int button, int action, int mods) {
    if (TouchInputRouter* router = RouterForTouchEvent(window)) {
        // GLFW button events carry no position, and a finger landing where the cursor already is
        // produces no cursor-move callback, so a press takes its point from the button message
        // itself. (A release needs nothing: GLFW reports any move before the button-up.)
        if (action == GLFW_PRESS) {
            float x = 0.0f;
            float y = 0.0f;
            PressPointInClient(window, x, y);
            router->OnRawMove(x, y);
        }
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
    g_propertySet = false;
    g_touchActive = false;
}

} // namespace standalone
} // namespace smatchet
