#pragma once

#include "SmatchetUiModeIds.h"
#include "TouchGesturePure.h"
#include "Ui/TouchSizingPure.h"

#include <functional>
#include <vector>

struct ImGuiContext;

// Desktop touchscreen support (docs/plans/touchscreen-support.md). One router per ImGui context,
// installed by the host right after ImGui::CreateContext (standalone main / StandaloneAppBootstrap,
// the Unreal SmatchetImGuiHost). Android does not install it — it has its own touch path.
//
// Hosts send FINGER input here instead of to ImGui (mouse input is unchanged): the router holds a
// press back until SmatchetTouchGesturePure::GestureMachine has classified it, then emits ImGui
// mouse events, scrolls the window under the finger, or opens the context menu with a synthetic
// right click. It runs from an ImGui NewFramePre context hook, i.e. before ImGui processes its
// input queue, while last frame's hover state still describes what is under the finger.
//
// Threading: every call (raw input, settings, hooks) must happen under whatever serializes the
// context's input with NewFrame — the UI thread in the standalone build, ImplData->ImGuiMutex in the
// Unreal host. The router keeps no other locks.
namespace smatchet {
namespace ui {
namespace touch {

class TouchInputRouter {
  public:
    // The router installed on `ctx`, or null when none is (Android, CLI-only contexts).
    static TouchInputRouter* ForContext(ImGuiContext* ctx);

    // Raw first-finger input in ImGui display coordinates. Call only for touchscreen-sourced events
    // and only while GesturesEnabled(); otherwise forward to ImGui as usual.
    void OnRawMove(float x, float y);
    void OnRawButton(int button, bool down);

    // True while the pointer is parked at the last tap so hover tooltips can show. The standalone
    // bridge swallows the touch cursor-leave that would otherwise move the pointer off-window.
    bool IsParked() const { return parked_; }

    // Settings, re-applied every frame by SmatchetUI from TrackerConfig.
    void SetMode(TouchInputMode mode);
    TouchInputMode Mode() const { return mode_; }
    bool GesturesEnabled() const { return mode_ != TouchInputMode::Off; }
    void SetLongPressSeconds(float seconds);
    void SetPixelScale(float scale);
    // Called with GesturesEnabled() whenever it changes (the standalone build toggles Windows'
    // own press-and-hold right click with it).
    void SetGesturesEnabledChangedCallback(std::function<void(bool)> callback);

    // Should hit targets be touch-sized now? On: always; Off: never; Auto: while a touchscreen is
    // in use. Only changes while no finger gesture is in progress (no layout shift under a finger).
    bool WantsTouchSizing() const;

    // Items whose drag must reach ImGui even inside a scrollable window (DnD sources, reorderable
    // headers). See MarkLastItemAsTouchDragSurface().
    void AddDragSurface(unsigned int itemId) { dragSurfacesCur_.push_back(itemId); }

    // --- hook entry points (called by the context hooks in TouchInputRouter.cpp) ---
    void PreNewFrame(ImGuiContext& g);
    void PostNewFrame(ImGuiContext& g);

  private:
    SmatchetTouchGesturePure::PressClass Classify(ImGuiContext& g, SmatchetTouchGesturePure::Vec2 p);
    void ScanQueue(ImGuiContext& g);
    void Emit(ImGuiContext& g);
    void ApplyPan(ImGuiContext& g, SmatchetTouchGesturePure::Vec2 delta);
    void CancelGesture(ImGuiContext& g);
    void ApplyConfig();

    SmatchetTouchGesturePure::GestureMachine machine_;
    SmatchetTouchSizingPure::TouchSizingDetector detector_;
    std::vector<SmatchetTouchGesturePure::Action> actions_;
    std::vector<unsigned int> dragSurfacesCur_;
    std::vector<unsigned int> dragSurfacesPrev_;
    // EventIds of the queue entries this router added; a touch-sourced position event that is not
    // one of them came from backend cursor polling and must not move the held-back pointer.
    std::vector<unsigned int> ownEventIds_;
    std::function<void(bool)> gesturesChanged_;
    TouchInputMode mode_ = TouchInputMode::Auto;
    float longPressSeconds_ = SmatchetTouchGesturePure::kDefaultLongPressSeconds;
    float pixelScale_ = 1.0f;
    double now_ = 0.0;
    unsigned int nextScanEventId_ = 0;
    unsigned int panTarget_[2] = {0, 0}; // ImGuiID of the window scrolled on X / Y
    float rawX_ = 0.0f;
    float rawY_ = 0.0f;
    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;
    bool lastMouseValid_ = false;
    bool rawDown_ = false;
    bool parked_ = false;
    bool sizingApplied_ = false;
};

// Install the router on `ctx` (no-op if one is already installed). The router is owned by the
// context and freed by its Shutdown hook.
void InstallTouchInputRouter(ImGuiContext* ctx);

// Re-register the router's hooks after every other hook. The ImGui test engine drops queued input
// it did not add in its own NewFramePre hook, so the router must run after it (UiTestScenario).
void MoveTouchRouterHooksToEnd(ImGuiContext* ctx);

// Mark the item just submitted as a drag surface: a finger drag that starts on it reaches ImGui as
// a drag instead of scrolling the enclosing window. Use for drag sources that live inside a
// scrollable window (a drag source NOT marked this way pans its window when touched). No-op
// without a router.
void MarkLastItemAsTouchDragSurface();

} // namespace touch
} // namespace ui
} // namespace smatchet
