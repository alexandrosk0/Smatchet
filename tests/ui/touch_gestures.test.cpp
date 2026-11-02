// touch_gestures.test.cpp — bucket-E coverage for desktop touchscreen gestures
// (Ui/TouchInputRouter.h, docs/plans/touchscreen-support.md). The pure classifier is unit-tested in
// tests/Core/TouchGesturePure.test.cpp; what only a live ImGui context can prove is the glue: that a
// held-back press turns into the right ImGui events at the right point, that a pan scrolls the
// window under the finger without activating anything, that a hold opens the existing right-click
// menu, that a splitter still drags, that the pointer stays parked for tooltips, that a mouse drag
// is untouched, and that hit targets grow while a touchscreen is in use.
//
// Finger input is injected the way the hosts do it — straight into the context's TouchInputRouter
// (OnRawMove / OnRawButton) — with ImGuiTestRunFlags_EnableRawInputs so the engine does not drive
// the mouse at the same time. Time advances in fixed 1/60 s steps (SleepNoSkip) so hold / fling
// timing is deterministic.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "Ui/TouchInputRouter.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

using smatchet::ui::touch::TouchInputRouter;

const char* const kWindowName = "SmatchetTest::TouchGestures";
// Leading "//" = absolute ref (see drag_checkbox_paint.test.cpp for why).
const char* const kWindowRef = "//SmatchetTest::TouchGestures";
constexpr float kStep = 1.0f / 60.0f;

struct TouchState {
    enum { kRows = 120 };
    int clicks[kRows];
    int doubleClicks[kRows];
    bool menuOpened[kRows];
    bool tooltipHover[kRows];
    ImVec2 centers[kRows];
    ImVec2 splitCenter;
    ImVec2 emptyPoint; // a spot in the test window outside the list (for mouse clicks)
    float split;
    float scrollY;
    bool resetScroll;
    bool geometryReady;
};

TouchState g_touch;

void ResetTouchState() {
    for (int i = 0; i < TouchState::kRows; ++i) {
        g_touch.clicks[i] = 0;
        g_touch.doubleClicks[i] = 0;
        g_touch.menuOpened[i] = false;
        g_touch.tooltipHover[i] = false;
        g_touch.centers[i] = ImVec2(0.0f, 0.0f);
    }
    g_touch.splitCenter = ImVec2(0.0f, 0.0f);
    g_touch.emptyPoint = ImVec2(0.0f, 0.0f);
    g_touch.split = 0.0f;
    g_touch.scrollY = 0.0f;
    g_touch.resetScroll = true;
    g_touch.geometryReady = false;
}

ImVec2 ItemCenter() {
    return ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                  (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
}

void TouchGuiFunc(ImGuiTestContext* ctx) {
    TouchState& s = *static_cast<TouchState*>(ctx->Test->UserData);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 320.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(60.0f, 60.0f), ImGuiCond_Always);
    if (ImGui::Begin(kWindowName, nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
        if (ImGui::BeginChild("##list", ImVec2(300.0f, 220.0f), ImGuiChildFlags_Borders)) {
            if (s.resetScroll) {
                ImGui::SetScrollY(0.0f);
                s.resetScroll = false;
            }
            // A splitter-style widget inside the scrollable list: it shows a resize cursor, so a
            // finger drag on it must reach it instead of scrolling the list.
            ImGui::InvisibleButton("##split", ImVec2(200.0f, 10.0f));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive()) {
                s.split += ImGui::GetIO().MouseDelta.x;
            }
            s.splitCenter = ItemCenter();
            for (int i = 0; i < TouchState::kRows; ++i) {
                char label[32];
                ImFormatString(label, IM_ARRAYSIZE(label), "row %d", i);
                ImGui::PushID(i); // each row gets its own context-menu popup id
                if (ImGui::Selectable(label)) {
                    ++s.clicks[i];
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    ++s.doubleClicks[i];
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                    s.tooltipHover[i] = true;
                }
                s.centers[i] = ItemCenter();
                if (ImGui::BeginPopupContextItem("##rowmenu")) {
                    s.menuOpened[i] = true;
                    ImGui::TextUnformatted("menu");
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            s.scrollY = ImGui::GetScrollY();
        }
        ImGui::EndChild();
        ImGui::TextUnformatted("outside the list");
        s.emptyPoint = ItemCenter();
    }
    ImGui::End();
    s.geometryReady = true;
}

TouchInputRouter* Router(ImGuiTestContext* ctx) { return TouchInputRouter::ForContext(ctx->UiContext); }

// Coarse step for idle waits: the router's timers and fling decay are exact in elapsed time, so
// waiting in 0.1 s frames asserts the same thing in a sixth of the frames (bucket-E wall-clock).
constexpr float kIdleStep = 0.1f;

// One frame of 1/60 s of ImGui time.
void Step(ImGuiTestContext* ctx, int frames = 1) {
    for (int i = 0; i < frames; ++i) {
        ctx->SleepNoSkip(kStep, kStep);
    }
}

void FingerDown(ImGuiTestContext* ctx, ImVec2 p) {
    Router(ctx)->OnRawMove(p.x, p.y);
    Router(ctx)->OnRawButton(0, true);
    Step(ctx);
}

void FingerUp(ImGuiTestContext* ctx) {
    Router(ctx)->OnRawButton(0, false);
    Step(ctx);
}

void FingerMoveBy(ImGuiTestContext* ctx, ImVec2& p, float dx, float dy, int steps) {
    for (int i = 0; i < steps; ++i) {
        p.x += dx;
        p.y += dy;
        Router(ctx)->OnRawMove(p.x, p.y);
        Step(ctx);
    }
}

void Tap(ImGuiTestContext* ctx, ImVec2 p) {
    FingerDown(ctx, p);
    FingerUp(ctx);
    Step(ctx, 3); // the press and the release land on separate frames (trickle queue)
}

// Shared prologue: window up and in front, geometry captured, raw (finger) input on. Returns false
// (after logging) when there is no router or gestures are switched off in this config.
bool Arm(ImGuiTestContext* ctx) {
    ResetTouchState();
    ctx->SetRef(kWindowRef);
    ctx->Yield(2);
    ctx->WindowFocus(kWindowRef);
    ctx->Yield();
    // The spawned test instance runs on a fresh config (Touch input = Auto). A missing router or
    // gestures switched Off would make every case below vacuous, so that is a failure, not a skip.
    IM_CHECK_NO_RET(Router(ctx) != nullptr);
    if (Router(ctx) == nullptr) {
        return false;
    }
    IM_CHECK_NO_RET(Router(ctx)->GesturesEnabled());
    if (!Router(ctx)->GesturesEnabled()) {
        return false;
    }
    ctx->RunFlags |= ImGuiTestRunFlags_EnableRawInputs;
    // A failed check returns from the TestFunc mid-gesture; lift any finger a previous test left down
    // so one failure cannot cascade into the next test.
    Router(ctx)->OnRawButton(0, false);
    Step(ctx, 2);
    return g_touch.geometryReady;
}

void Disarm(ImGuiTestContext* ctx) {
    ctx->PopupCloseAll();
    ctx->RunFlags &= ~ImGuiTestRunFlags_EnableRawInputs;
    ctx->Yield(2);
}

ImGuiTest* Register(ImGuiTestEngine* engine, const char* name) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TouchGestures", name);
    t->UserData = &g_touch;
    t->GuiFunc = TouchGuiFunc;
    return t;
}

void RegisterTapSelectsOnce(ImGuiTestEngine* engine) {
    Register(engine, "TapClicksOnceAtThePressPoint")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        Tap(ctx, g_touch.centers[2]);
        IM_CHECK_EQ(g_touch.clicks[2], 1);
        IM_CHECK_EQ(g_touch.clicks[1], 0);
        IM_CHECK_EQ(g_touch.clicks[3], 0);
        IM_CHECK_EQ(g_touch.scrollY, 0.0f);
        Disarm(ctx);
    };
}

void RegisterLongPressOpensMenu(ImGuiTestEngine* engine) {
    Register(engine, "HoldOpensContextMenuWithoutClicking")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        FingerDown(ctx, g_touch.centers[3]);
        ctx->SleepNoSkip(SmatchetTouchGesturePure::kMaxLongPressSeconds + 0.1f, kIdleStep);
        IM_CHECK(g_touch.menuOpened[3]);
        IM_CHECK_EQ(g_touch.clicks[3], 0); // the left button was never sent
        FingerUp(ctx);
        Step(ctx, 2);
        IM_CHECK_EQ(g_touch.clicks[3], 0); // and the release did not click either
        Disarm(ctx);
    };
}

void RegisterDragScrolls(ImGuiTestEngine* engine) {
    Register(engine, "DragScrollsTheListWithoutActivating")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        ImVec2 p = g_touch.centers[8];
        FingerDown(ctx, p);
        FingerMoveBy(ctx, p, 0.0f, -10.0f, 12); // 120 px up, slowly
        Step(ctx, 15);                          // rest before lifting: no fling
        FingerUp(ctx);
        Step(ctx, 3);
        IM_CHECK_GT(g_touch.scrollY, 110.0f);
        IM_CHECK_LT(g_touch.scrollY, 130.0f);
        for (int i = 0; i < TouchState::kRows; ++i) {
            IM_CHECK_EQ(g_touch.clicks[i], 0);
        }
        const float settled = g_touch.scrollY;
        Step(ctx, 10);
        IM_CHECK_EQ(g_touch.scrollY, settled);
        Disarm(ctx);
    };
}

void RegisterFling(ImGuiTestEngine* engine) {
    Register(engine, "FastReleaseFlingsThenStops")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        ImVec2 p = g_touch.centers[8];
        FingerDown(ctx, p);
        FingerMoveBy(ctx, p, 0.0f, -25.0f, 5); // ~1500 px/s
        FingerUp(ctx);
        const float atRelease = g_touch.scrollY;
        ctx->SleepNoSkip(0.3f, kStep);
        IM_CHECK_GT(g_touch.scrollY, atRelease + 50.0f);
        ctx->SleepNoSkip(3.0f, kIdleStep);
        const float stopped = g_touch.scrollY;
        Step(ctx, 5);
        IM_CHECK_EQ(g_touch.scrollY, stopped);
        for (int i = 0; i < TouchState::kRows; ++i) {
            IM_CHECK_EQ(g_touch.clicks[i], 0);
        }
        Disarm(ctx);
    };
}

void RegisterSplitterDragThrough(ImGuiTestEngine* engine) {
    Register(engine, "SplitterInsideListStillDrags")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        ImVec2 p = g_touch.splitCenter;
        Router(ctx)->OnRawMove(p.x, p.y); // finger hovers first so the resize cursor is up
        Step(ctx, 2);
        FingerDown(ctx, p);
        Step(ctx);
        FingerMoveBy(ctx, p, 10.0f, 0.0f, 4);
        FingerUp(ctx);
        Step(ctx, 2);
        IM_CHECK_GT(g_touch.split, 30.0f);
        IM_CHECK_EQ(g_touch.scrollY, 0.0f);
        Disarm(ctx);
    };
}

void RegisterTapShowsTooltip(ImGuiTestEngine* engine) {
    Register(engine, "TapParksThePointerForTooltips")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        Tap(ctx, g_touch.centers[1]);
        g_touch.tooltipHover[1] = false;
        ctx->SleepNoSkip(0.6f, kStep);
        IM_CHECK(Router(ctx)->IsParked());
        IM_CHECK(g_touch.tooltipHover[1]);
        Disarm(ctx);
    };
}

void RegisterDoubleTap(ImGuiTestEngine* engine) {
    Register(engine, "DoubleTapIsADoubleClick")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        const ImVec2 p = g_touch.centers[4];
        FingerDown(ctx, p);
        FingerUp(ctx);
        Step(ctx);
        FingerDown(ctx, ImVec2(p.x + 6.0f, p.y + 3.0f)); // a finger never lands twice on one pixel
        FingerUp(ctx);
        Step(ctx, 3);
        IM_CHECK_EQ(g_touch.doubleClicks[4], 1);
        Disarm(ctx);
    };
}

void RegisterMouseDragDoesNotPan(ImGuiTestEngine* engine) {
    Register(engine, "MouseDragNeverPans")->TestFunc = [](ImGuiTestContext* ctx) {
        ResetTouchState();
        ctx->SetRef(kWindowRef);
        ctx->Yield(2);
        ctx->WindowFocus(kWindowRef);
        ctx->Yield();
        // Engine-driven (mouse-sourced) input: the router must stay out of the way.
        ctx->MouseMoveToPos(g_touch.centers[8]);
        ctx->MouseDown(0);
        ctx->MouseMoveToPos(ImVec2(g_touch.centers[8].x, g_touch.centers[8].y - 100.0f));
        ctx->MouseUp(0);
        ctx->Yield(3);
        IM_CHECK_EQ(g_touch.scrollY, 0.0f);

        // And a plain mouse click still clicks exactly once, with the router idle and not holding
        // the pointer (its queue scan and source-latch restore must not disturb mouse input).
        ctx->MouseMoveToPos(g_touch.centers[5]);
        ctx->MouseClick(0);
        ctx->Yield(3);
        IM_CHECK_EQ(g_touch.clicks[5], 1);
        IM_CHECK(Router(ctx) == nullptr || !Router(ctx)->IsParked());
    };
}

// The test engine snapshots ImGuiStyle when a test starts and restores it when it ends, which hides
// any style SmatchetUI applied in between from its own "already applied" bookkeeping. So this test
// drives sizing explicitly off and then on (and off again at the end) instead of assuming a start
// state, and it runs last so the earlier tests' taps do not matter.
void MouseTakesOver(ImGuiTestContext* ctx) {
    ctx->SleepNoSkip(2.2f, kIdleStep); // past the touch dwell
    ctx->RunFlags &= ~ImGuiTestRunFlags_EnableRawInputs;
    ctx->MouseMoveToPos(g_touch.emptyPoint);
    ctx->MouseClick(0);
    ctx->Yield(3);
}

void RegisterTouchSizing(ImGuiTestEngine* engine) {
    Register(engine, "HitTargetsGrowWhileTouching")->TestFunc = [](ImGuiTestContext* ctx) {
        if (!Arm(ctx)) {
            return;
        }
        if (Router(ctx)->Mode() != TouchInputMode::Auto) {
            ctx->LogWarning("Touch input is not Auto in this config; skipping the Auto sizing check");
            Disarm(ctx);
            return;
        }
        MouseTakesOver(ctx);
        IM_CHECK(!Router(ctx)->WantsTouchSizing());
        IM_CHECK_EQ(ImGui::GetStyle().TouchExtraPadding.x, 0.0f);

        ctx->RunFlags |= ImGuiTestRunFlags_EnableRawInputs;
        Tap(ctx, g_touch.emptyPoint);
        Step(ctx, 3);
        IM_CHECK(Router(ctx)->WantsTouchSizing());
        IM_CHECK_GT(ImGui::GetStyle().TouchExtraPadding.x, 0.0f);
        IM_CHECK_GE(ImGui::GetStyle().ScrollbarSize, SmatchetTouchSizingPure::kTouchScrollbarSizePx);

        // Mouse use straight after a touch keeps the touch layout...
        ctx->RunFlags &= ~ImGuiTestRunFlags_EnableRawInputs;
        ctx->MouseClick(0);
        ctx->Yield(3);
        IM_CHECK(Router(ctx)->WantsTouchSizing());
        // ...a click once the dwell has passed restores the mouse layout.
        MouseTakesOver(ctx);
        IM_CHECK(!Router(ctx)->WantsTouchSizing());
        IM_CHECK_EQ(ImGui::GetStyle().TouchExtraPadding.x, 0.0f);
    };
}

} // namespace

extern "C" void SmatchetRegisterTouchGestureTests(ImGuiTestEngine* engine) {
    RegisterTapSelectsOnce(engine);
    RegisterLongPressOpensMenu(engine);
    RegisterDragScrolls(engine);
    RegisterFling(engine);
    RegisterSplitterDragThrough(engine);
    RegisterTapShowsTooltip(engine);
    RegisterDoubleTap(engine);
    RegisterMouseDragDoesNotPan(engine);
    RegisterTouchSizing(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
