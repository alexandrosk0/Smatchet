// Desktop touchscreen gesture router — the ImGui glue around SmatchetTouchGesturePure::GestureMachine.
// See Ui/TouchInputRouter.h for the contract and docs/plans/touchscreen-support.md for the design.

#include "Ui/TouchInputRouter.h"

#include "Logger.h"

#include "imgui.h"
#include "imgui_internal.h" // context hooks, input queue, HoveredWindow/HoveredId, window scroll state

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <utility>

namespace smatchet {
namespace ui {
namespace touch {

namespace {

using SmatchetTouchGesturePure::Action;
using SmatchetTouchGesturePure::ActionType;
using SmatchetTouchGesturePure::PressClass;
using SmatchetTouchGesturePure::Vec2;

// Hook owner tag ('TCHR'); identifies this router's hooks in ImGuiContext::Hooks.
const ImGuiID kTouchRouterHookOwner = 0x54434852u;

// Progress ring drawn under a finger that is holding still, before the context menu opens.
constexpr float kRingRadiusPx = 22.0f;
constexpr float kRingThicknessPx = 3.0f;
constexpr float kRingAlpha = 0.6f;

void PreNewFrameHook(ImGuiContext* ctx, ImGuiContextHook* hook) {
    static_cast<TouchInputRouter*>(hook->UserData)->PreNewFrame(*ctx);
}

void PostNewFrameHook(ImGuiContext* ctx, ImGuiContextHook* hook) {
    static_cast<TouchInputRouter*>(hook->UserData)->PostNewFrame(*ctx);
}

void ShutdownHook(ImGuiContext*, ImGuiContextHook* hook) {
    // Take back the ownership InstallTouchInputRouter released into the hook; freed on scope exit.
    std::unique_ptr<TouchInputRouter> owned(static_cast<TouchInputRouter*>(hook->UserData));
    hook->UserData = nullptr;
}

void AddRouterHooks(ImGuiContext* ctx, TouchInputRouter* router) {
    const ImGuiContextHookType types[] = {ImGuiContextHookType_NewFramePre, ImGuiContextHookType_NewFramePost,
                                          ImGuiContextHookType_Shutdown};
    const ImGuiContextHookCallback callbacks[] = {PreNewFrameHook, PostNewFrameHook, ShutdownHook};
    for (int i = 0; i < 3; ++i) {
        ImGuiContextHook hook; // zero-initialised by its constructor (HookId must be 0)
        hook.Type = types[i];
        hook.Owner = kTouchRouterHookOwner;
        hook.Callback = callbacks[i];
        hook.UserData = router;
        ImGui::AddContextHook(ctx, &hook);
    }
}

bool IsResizeOrTextCursor(ImGuiMouseCursor cursor) {
    switch (cursor) {
    case ImGuiMouseCursor_TextInput:
    case ImGuiMouseCursor_ResizeAll:
    case ImGuiMouseCursor_ResizeNS:
    case ImGuiMouseCursor_ResizeEW:
    case ImGuiMouseCursor_ResizeNESW:
    case ImGuiMouseCursor_ResizeNWSE:
        return true;
    default:
        return false;
    }
}

bool TabBarContains(const ImGuiDockNode* node, const ImVec2& pt) {
    return node != nullptr && node->TabBar != nullptr && node->TabBar->BarRect.Contains(pt);
}

ImGuiMouseSource EventMouseSource(const ImGuiInputEvent& e) {
    switch (e.Type) {
    case ImGuiInputEventType_MousePos:
        return e.MousePos.MouseSource;
    case ImGuiInputEventType_MouseButton:
        return e.MouseButton.MouseSource;
    case ImGuiInputEventType_MouseWheel:
        return e.MouseWheel.MouseSource;
    default:
        return ImGuiMouseSource_Mouse;
    }
}

} // namespace

struct TouchInputRouter::TickGlue {
    TouchInputRouter& router;
    ImGuiContext& g;
    PressClass Classify(Vec2 p) { return router.Classify(g, p); }
    // A quick second tap is a double-tap only on the item the first tap pressed. HoveredId is still
    // last frame's here, which (pointer held at the second press point) is the item under it.
    bool SameTarget(Vec2, Vec2) { return router.lastLeftDownItem_ != 0 && g.HoveredId == router.lastLeftDownItem_; }
};

TouchInputRouter* TouchInputRouter::ForContext(ImGuiContext* ctx) {
    if (ctx == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < ctx->Hooks.Size; ++i) {
        const ImGuiContextHook& hook = ctx->Hooks[i];
        if (hook.Owner == kTouchRouterHookOwner && hook.Type == ImGuiContextHookType_Shutdown) {
            return static_cast<TouchInputRouter*>(hook.UserData);
        }
    }
    return nullptr;
}

void TouchInputRouter::OnRawMove(float x, float y) {
    rawX_ = x;
    rawY_ = y;
    machine_.OnMove(SmatchetTouchGesturePure::MakeVec2(x, y));
}

void TouchInputRouter::OnRawButton(int button, bool down) {
    // Only the first finger's primary contact drives gestures. Windows' own press-and-hold right
    // click (touch-sourced button 1) is dropped: the router produces the context menu itself.
    if (button != 0) {
        return;
    }
    if (down == rawDown_) {
        if (!down) {
            return;
        }
        // The previous release never arrived (overlay hidden or capture lost mid-gesture). End that
        // gesture, releasing anything it pressed, before starting this one.
        machine_.Cancel(actions_);
    }
    rawDown_ = down;
    const Vec2 p = SmatchetTouchGesturePure::MakeVec2(rawX_, rawY_);
    if (down) {
        detector_.OnTouch(now_);
        machine_.OnDown(p);
    } else {
        machine_.OnUp(p);
    }
}

void TouchInputRouter::SetMode(TouchInputMode mode) {
    const bool wasEnabled = GesturesEnabled();
    mode_ = mode;
    if (wasEnabled != GesturesEnabled()) {
        LOG_INFO("TouchInputRouter: touch gestures %s (touch input = %s)", GesturesEnabled() ? "on" : "off",
                 touchInputModeToString(mode));
        if (gesturesChanged_) {
            gesturesChanged_(GesturesEnabled());
        }
    }
}

void TouchInputRouter::SetLongPressSeconds(float seconds) {
    const float clamped = SmatchetTouchGesturePure::ClampLongPressSeconds(seconds);
    if (clamped != longPressSeconds_) {
        longPressSeconds_ = clamped;
        ApplyConfig();
    }
}

void TouchInputRouter::SetPixelScale(float scale) {
    const float s = (scale > 0.0f) ? scale : 1.0f;
    if (s != pixelScale_) {
        pixelScale_ = s;
        ApplyConfig();
    }
}

void TouchInputRouter::ApplyConfig() {
    SmatchetTouchGesturePure::GestureConfig base;
    base.longPressSeconds = longPressSeconds_;
    machine_.SetConfig(SmatchetTouchGesturePure::ScaledBy(base, pixelScale_));
}

void TouchInputRouter::SetGesturesEnabledChangedCallback(std::function<void(bool)> callback) {
    gesturesChanged_ = std::move(callback);
}

bool TouchInputRouter::WantsTouchSizing() const {
    switch (mode_) {
    case TouchInputMode::On:
        return true;
    case TouchInputMode::Off:
        return false;
    case TouchInputMode::Auto:
        return sizingApplied_;
    }
    return false;
}

void TouchInputRouter::PreNewFrame(ImGuiContext& g) {
    now_ += (g.IO.DeltaTime > 0.0f) ? static_cast<double>(g.IO.DeltaTime) : 0.0;
    // Marks recorded while drawing the frame that just ended describe the hover state classification
    // reads now (also last frame's).
    dragSurfacesPrev_.swap(dragSurfacesCur_);
    dragSurfacesCur_.clear();

    ScanQueue(g);
    if (GesturesEnabled()) {
        TickGlue glue{*this, g};
        machine_.Tick(now_, glue, actions_);
    } else if (!machine_.IsIdle()) {
        machine_.Cancel(actions_); // switched Off mid-gesture: release anything already pressed
    }
    Emit(g);
    if (machine_.IsIdle()) {
        sizingApplied_ = detector_.Active();
    }
}

// Walks ImGui's pending input queue once per frame:
//  - mouse (non-touch) activity feeds the sizing detector, ends the parked pointer and cancels a
//    finger gesture in flight, because the mouse wins.
//  - touchscreen position events this router did not add are dropped while gestures are on. They
//    come from the GLFW backend polling the OS cursor (which Windows moves to the finger) and would
//    otherwise drag the held-back pointer along with the finger.
void TouchInputRouter::ScanQueue(ImGuiContext& g) {
    bool mouseTookOver = false;
    bool focusLost = false;
    for (int i = 0; i < g.InputEventsQueue.Size; ++i) {
        const ImGuiInputEvent& e = g.InputEventsQueue[i];
        if (e.Type == ImGuiInputEventType_Focus) {
            focusLost = focusLost || (e.EventId >= nextScanEventId_ && !e.AppFocused.Focused);
            continue;
        }
        if (e.Type != ImGuiInputEventType_MousePos && e.Type != ImGuiInputEventType_MouseButton &&
            e.Type != ImGuiInputEventType_MouseWheel) {
            continue;
        }
        const bool own = std::find(ownEventIds_.begin(), ownEventIds_.end(), e.EventId) != ownEventIds_.end();
        if (own) {
            continue;
        }
        const bool touch = EventMouseSource(e) == ImGuiMouseSource_TouchScreen;
        if (touch && e.Type == ImGuiInputEventType_MousePos && GesturesEnabled() && !e.AddedByTestEngine) {
            g.InputEventsQueue.erase(&g.InputEventsQueue[i]);
            --i;
            continue;
        }
        if (e.EventId < nextScanEventId_) {
            continue; // already seen on an earlier frame (left in the queue by the trickle rules)
        }
        if (touch) {
            if (e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Down) {
                detector_.OnTouch(now_); // touch reaching ImGui directly (gestures Off)
            }
            continue;
        }
        parked_ = false;
        if (e.Type == ImGuiInputEventType_MousePos) {
            const float x = e.MousePos.PosX;
            const float y = e.MousePos.PosY;
            if (x > -FLT_MAX && y > -FLT_MAX) {
                if (lastMouseValid_) {
                    detector_.OnMouseMove(
                        now_, std::sqrt((x - lastMouseX_) * (x - lastMouseX_) + (y - lastMouseY_) * (y - lastMouseY_)));
                }
                lastMouseX_ = x;
                lastMouseY_ = y;
                lastMouseValid_ = true;
            }
        } else {
            detector_.OnMouseButtonOrWheel(now_);
            mouseTookOver = true;
        }
    }
    nextScanEventId_ = g.InputEventsNextEventId;

    // Forget own ids that ImGui has already consumed.
    ownEventIds_.erase(std::remove_if(ownEventIds_.begin(), ownEventIds_.end(),
                                      [&g](unsigned int id) {
                                          for (int i = 0; i < g.InputEventsQueue.Size; ++i) {
                                              if (g.InputEventsQueue[i].EventId == id) {
                                                  return false;
                                              }
                                          }
                                          return true;
                                      }),
                       ownEventIds_.end());

    if ((mouseTookOver || focusLost) && !machine_.IsIdle()) {
        machine_.Cancel(actions_);
    }
    if (focusLost) {
        // The window lost focus: a finger release may never arrive, so do not wait for it.
        rawDown_ = false;
        parked_ = false;
    }
}

void TouchInputRouter::Emit(ImGuiContext& g) {
    if (actions_.empty()) {
        return;
    }
    ImGuiIO& io = g.IO;
    // Tag our events as touch (ImGui then delays a press until its position has been seen, offsets
    // tooltips from the finger, etc.) and restore the backend's latch afterwards, so input the
    // backend or the test engine queues next keeps its own source.
    const ImGuiMouseSource savedSource = g.InputEventsNextMouseSource;
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    const unsigned int firstId = g.InputEventsNextEventId;
    for (const Action& a : actions_) {
        switch (a.type) {
        case ActionType::MovePointer:
            io.AddMousePosEvent(a.pos.x, a.pos.y);
            parked_ = true;
            break;
        case ActionType::HidePointer:
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            parked_ = false;
            break;
        case ActionType::LeftDown:
            lastLeftDownItem_ = g.HoveredId; // pointer is already at the press point (held there)
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            break;
        case ActionType::LeftUp:
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            break;
        case ActionType::RightDown:
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            break;
        case ActionType::RightUp:
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
            break;
        case ActionType::PanBy:
            ApplyPan(g, a.delta);
            break;
        case ActionType::PanEnd:
            panTarget_[0] = 0;
            panTarget_[1] = 0;
            break;
        }
    }
    for (unsigned int id = firstId; id != g.InputEventsNextEventId; ++id) {
        ownEventIds_.push_back(id);
    }
    // ImGui's trickle rules can leave part of what was just queued (a tap's release, a drag's first
    // move) for the next frame. The ImGui test engine erases queued events it did not add at the
    // start of every frame while it drives input, which would drop those leftovers and leave a button
    // held. AddedByTestEngine is only read by the test engine (ImGui core ignores it), so flagging
    // our own events keeps them alive there and changes nothing in the shipping app.
    for (int i = 0; i < g.InputEventsQueue.Size; ++i) {
        if (g.InputEventsQueue[i].EventId >= firstId) {
            g.InputEventsQueue[i].AddedByTestEngine = true;
        }
    }
    nextScanEventId_ = g.InputEventsNextEventId;
    io.AddMouseSourceEvent(savedSource);
    actions_.clear();
}

// Scroll the target window(s) chosen at classification so the content follows the finger. Applied
// through SetScrollX/Y before the windows Begin this frame (same approach as the tooltip wheel
// router), so no wheel events are generated and the grid's own wheel routing is not involved.
void TouchInputRouter::ApplyPan(ImGuiContext& g, Vec2 delta) {
    (void)g;
    const float d[2] = {delta.x, delta.y};
    for (int axis = 0; axis < 2; ++axis) {
        if (panTarget_[axis] == 0 || d[axis] == 0.0f) {
            continue;
        }
        ImGuiWindow* w = ImGui::FindWindowByID(panTarget_[axis]);
        if (w == nullptr || !w->Active) { // Active = submitted last frame (WasActive lags a frame more)
            panTarget_[axis] = 0;
            machine_.OnPanAxisBlocked(axis == 0);
            continue;
        }
        const float pending = (axis == 0) ? w->ScrollTarget.x : w->ScrollTarget.y;
        const float current = (pending < FLT_MAX) ? pending : ((axis == 0) ? w->Scroll.x : w->Scroll.y);
        const float maxScroll = (axis == 0) ? w->ScrollMax.x : w->ScrollMax.y;
        const float next = ImClamp(current - d[axis], 0.0f, maxScroll);
        if (next == current) {
            machine_.OnPanAxisBlocked(axis == 0); // edge reached: a fling stops on this axis
            continue;
        }
        if (axis == 0) {
            ImGui::SetScrollX(w, next);
        } else {
            ImGui::SetScrollY(w, next);
        }
    }
}

// What does a drag that starts at `p` mean? Reads last frame's hover state, which describes `p`
// because the pointer has been held at the press point since the press (the machine never
// classifies in the same tick as the press).
PressClass TouchInputRouter::Classify(ImGuiContext& g, Vec2 p) {
    panTarget_[0] = 0;
    panTarget_[1] = 0;
    ImGuiWindow* hovered = g.HoveredWindow;
    if (hovered == nullptr) {
        return PressClass::DragThrough;
    }
    // Behind an open popup or modal: a drag there is ambiguous, so ignore it. (A tap is not
    // classified at all and still closes the popup the usual way.)
    if (g.OpenPopupStack.Size > 0) {
        bool insidePopup = false;
        for (int i = 0; i < g.OpenPopupStack.Size && !insidePopup; ++i) {
            ImGuiWindow* popup = g.OpenPopupStack[i].Window;
            insidePopup = popup != nullptr && ImGui::IsWindowChildOf(hovered, popup, true, false);
        }
        if (!insidePopup) {
            return PressClass::Blocked;
        }
    }
    // Splitters, column / window resize borders and text fields show a resize or text cursor on
    // hover; the drag belongs to them.
    if (IsResizeOrTextCursor(g.MouseCursor)) {
        return PressClass::DragThrough;
    }
    const ImVec2 pt(p.x, p.y);
    if (g.HoveredId != 0 &&
        std::find(dragSurfacesPrev_.begin(), dragSurfacesPrev_.end(), g.HoveredId) != dragSurfacesPrev_.end()) {
        return PressClass::DragThrough;
    }
    ImGuiWindow* scrollX = nullptr;
    ImGuiWindow* scrollY = nullptr;
    for (ImGuiWindow* w = hovered; w != nullptr; w = w->ParentWindow) {
        if (g.HoveredId != 0 && (g.HoveredId == ImGui::GetWindowScrollbarID(w, ImGuiAxis_X) ||
                                 g.HoveredId == ImGui::GetWindowScrollbarID(w, ImGuiAxis_Y))) {
            return PressClass::DragThrough; // dragging a scrollbar thumb
        }
        // Title bar, menu bar and dock tab bar drags move / undock windows. (Both rects are empty
        // for a window without that bar.)
        if (w->TitleBarRect().Contains(pt) || w->MenuBarRect().Contains(pt) || TabBarContains(w->DockNode, pt) ||
            TabBarContains(w->DockNodeAsHost, pt)) {
            return PressClass::DragThrough;
        }
        if (scrollX == nullptr && w->ScrollMax.x > 0.0f) {
            scrollX = w;
        }
        if (scrollY == nullptr && w->ScrollMax.y > 0.0f) {
            scrollY = w;
        }
        if ((w->Flags & ImGuiWindowFlags_ChildWindow) == 0) {
            break;
        }
    }
    if (scrollX == nullptr && scrollY == nullptr) {
        return PressClass::DragThrough; // nothing to scroll: behave like a mouse drag
    }
    panTarget_[0] = scrollX ? scrollX->ID : 0;
    panTarget_[1] = scrollY ? scrollY->ID : 0;
    return PressClass::Pannable;
}

void TouchInputRouter::PostNewFrame(ImGuiContext& g) {
    (void)g;
    const float progress = machine_.LongPressProgress(now_);
    if (progress <= 0.0f) {
        return;
    }
    const Vec2 p = machine_.PressPos();
    const float startAngle = -IM_PI * 0.5f;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->PathArcTo(ImVec2(p.x, p.y), kRingRadiusPx * pixelScale_, startAngle, startAngle + 2.0f * IM_PI * progress,
                        32);
    drawList->PathStroke(ImGui::GetColorU32(ImGuiCol_Text, kRingAlpha), ImDrawFlags_None,
                         kRingThicknessPx * pixelScale_);
}

void InstallTouchInputRouter(ImGuiContext* ctx) {
    if (ctx == nullptr || TouchInputRouter::ForContext(ctx) != nullptr) {
        return;
    }
    // The context owns the router from here on: ShutdownHook takes it back into a unique_ptr.
    AddRouterHooks(ctx, std::make_unique<TouchInputRouter>().release());
}

void MoveTouchRouterHooksToEnd(ImGuiContext* ctx) {
    TouchInputRouter* router = TouchInputRouter::ForContext(ctx);
    if (router == nullptr) {
        return;
    }
    for (int i = 0; i < ctx->Hooks.Size; ++i) {
        const ImGuiContextHook& hook = ctx->Hooks[i];
        if (hook.Owner == kTouchRouterHookOwner && hook.Type != ImGuiContextHookType_PendingRemoval_) {
            ImGui::RemoveContextHook(ctx, hook.HookId); // marked; erased at the next NewFrame
        }
    }
    AddRouterHooks(ctx, router);
}

void MarkLastItemAsTouchDragSurface() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    TouchInputRouter* router = TouchInputRouter::ForContext(g);
    if (router != nullptr && g->LastItemData.ID != 0) {
        router->AddDragSurface(g->LastItemData.ID);
    }
}

} // namespace touch
} // namespace ui
} // namespace smatchet
