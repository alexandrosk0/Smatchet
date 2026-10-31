// Pure-logic coverage for the desktop touchscreen gesture machine (TouchGesturePure.h). Pins the
// contract the ImGui glue (Ui/TouchInputRouter.cpp) relies on: a press is held back until it is
// classified, a tap becomes a left click at the press point, a stationary hold becomes a right
// click with no left press at all, a pan never presses a button, a widget drag forwards the press
// at its original point, and a fast release flings with frame-rate-independent decay.

#include "TouchGesturePure.h"

#include <doctest/doctest.h>

#include <vector>

using namespace SmatchetTouchGesturePure;

namespace {

constexpr double kFrame = 1.0 / 60.0;

// Stands in for the ImGui glue: a fixed answer for "what does a drag here mean" and for "did the
// second tap hit the first tap's item".
struct FakeGlue {
    PressClass cls;
    int* calls;
    bool sameTarget;
    PressClass Classify(Vec2) const {
        if (calls) {
            ++*calls;
        }
        return cls;
    }
    bool SameTarget(Vec2, Vec2) const { return sameTarget; }
};

std::vector<Action> TickAs(GestureMachine& m, double now, PressClass cls, int* calls = nullptr,
                           bool sameTarget = true) {
    std::vector<Action> out;
    FakeGlue glue{cls, calls, sameTarget};
    m.Tick(now, glue, out);
    return out;
}

int Count(const std::vector<Action>& actions, ActionType type) {
    int n = 0;
    for (const Action& a : actions) {
        if (a.type == type) {
            ++n;
        }
    }
    return n;
}

bool HasButton(const std::vector<Action>& actions) {
    return Count(actions, ActionType::LeftDown) + Count(actions, ActionType::LeftUp) +
               Count(actions, ActionType::RightDown) + Count(actions, ActionType::RightUp) >
           0;
}

Vec2 V(float x, float y) { return MakeVec2(x, y); }

} // namespace

TEST_SUITE("TouchGesturePure") {
    TEST_CASE("a press is held back and a tap becomes a left click at the press point") {
        GestureMachine m;
        m.OnDown(V(100, 100));
        const std::vector<Action> down = TickAs(m, kFrame, PressClass::Pannable);
        CHECK_FALSE(HasButton(down));
        REQUIRE_EQ(Count(down, ActionType::MovePointer), 1);
        CHECK_EQ(m.State(), GestureState::Pending);

        m.OnMove(V(104, 103)); // inside the slop: still a tap
        m.OnUp(V(104, 103));
        const std::vector<Action> up = TickAs(m, 2 * kFrame, PressClass::Pannable);
        REQUIRE_EQ(up.size(), 2u);
        CHECK_EQ(up[0].type, ActionType::LeftDown);
        CHECK_EQ(up[0].pos.x, 100.0f);
        CHECK_EQ(up[0].pos.y, 100.0f);
        CHECK_EQ(up[1].type, ActionType::LeftUp);
        CHECK(m.IsIdle());
    }

    TEST_CASE("a pan never presses a button and scrolls by the finger delta") {
        GestureMachine m;
        int calls = 0;
        m.OnDown(V(100, 100));
        TickAs(m, kFrame, PressClass::Pannable, &calls);
        m.OnMove(V(100, 140));
        std::vector<Action> a = TickAs(m, 2 * kFrame, PressClass::Pannable, &calls);
        CHECK_EQ(calls, 1);
        CHECK_EQ(m.State(), GestureState::Pan);
        CHECK_EQ(Count(a, ActionType::HidePointer), 1);
        float total = 0.0f;
        for (const Action& x : a) {
            if (x.type == ActionType::PanBy) {
                total += x.delta.y;
            }
        }
        CHECK_EQ(total, doctest::Approx(40.0f));

        m.OnMove(V(100, 150));
        a = TickAs(m, 3 * kFrame, PressClass::Pannable, &calls);
        CHECK_EQ(calls, 1); // classified once per press
        REQUIRE_EQ(Count(a, ActionType::PanBy), 1);
        CHECK_EQ(a.back().delta.y, doctest::Approx(10.0f));
        CHECK_FALSE(HasButton(a));
    }

    TEST_CASE("a mostly-vertical pan locks to the vertical axis") {
        GestureMachine m;
        m.OnDown(V(100, 100));
        TickAs(m, kFrame, PressClass::Pannable);
        m.OnMove(V(103, 140));
        TickAs(m, 2 * kFrame, PressClass::Pannable);
        m.OnMove(V(130, 160)); // sideways drift after the lock
        const std::vector<Action> a = TickAs(m, 3 * kFrame, PressClass::Pannable);
        REQUIRE_EQ(Count(a, ActionType::PanBy), 1);
        CHECK_EQ(a.back().delta.x, 0.0f);
        CHECK_EQ(a.back().delta.y, doctest::Approx(20.0f));
    }

    TEST_CASE("classification waits a tick so hover reflects the press point") {
        GestureMachine m;
        int calls = 0;
        m.OnDown(V(10, 10));
        m.OnMove(V(10, 80)); // flick within the same tick as the press
        TickAs(m, kFrame, PressClass::Pannable, &calls);
        CHECK_EQ(calls, 0);
        CHECK_EQ(m.State(), GestureState::Pending);
        TickAs(m, 2 * kFrame, PressClass::Pannable, &calls);
        CHECK_EQ(calls, 1);
        CHECK_EQ(m.State(), GestureState::Pan);
    }

    TEST_CASE("a widget drag forwards the press at its original point before the move") {
        GestureMachine m;
        m.OnDown(V(50, 50));
        TickAs(m, kFrame, PressClass::DragThrough);
        m.OnMove(V(90, 50));
        std::vector<Action> a = TickAs(m, 2 * kFrame, PressClass::DragThrough);
        REQUIRE_EQ(a.size(), 2u);
        CHECK_EQ(a[0].type, ActionType::LeftDown);
        CHECK_EQ(a[0].pos.x, 50.0f);
        CHECK_EQ(a[1].type, ActionType::MovePointer);
        CHECK_EQ(a[1].pos.x, 90.0f);

        m.OnMove(V(120, 50));
        m.OnUp(V(120, 50));
        a = TickAs(m, 3 * kFrame, PressClass::DragThrough);
        CHECK_EQ(Count(a, ActionType::LeftUp), 1);
        CHECK(m.IsIdle());
    }

    TEST_CASE("a blocked press emits nothing, including on release") {
        GestureMachine m;
        m.OnDown(V(50, 50));
        TickAs(m, kFrame, PressClass::Blocked);
        m.OnMove(V(50, 120));
        m.OnUp(V(50, 120));
        const std::vector<Action> a = TickAs(m, 2 * kFrame, PressClass::Blocked);
        CHECK(a.empty());
        CHECK(m.IsIdle());
    }

    TEST_CASE("a stationary hold becomes a right click and the release is swallowed") {
        GestureMachine m;
        m.OnDown(V(70, 70));
        TickAs(m, kFrame, PressClass::Pannable);
        CHECK(TickAs(m, kFrame + 0.3, PressClass::Pannable).empty());
        CHECK_GT(m.LongPressProgress(kFrame + 0.3), 0.0f);

        const std::vector<Action> a = TickAs(m, kFrame + kDefaultLongPressSeconds + 0.001, PressClass::Pannable);
        REQUIRE_EQ(a.size(), 2u);
        CHECK_EQ(a[0].type, ActionType::RightDown);
        CHECK_EQ(a[0].pos.x, 70.0f);
        CHECK_EQ(a[1].type, ActionType::RightUp);
        CHECK_EQ(Count(a, ActionType::LeftDown), 0);

        m.OnUp(V(70, 70));
        CHECK(TickAs(m, kFrame + 0.6, PressClass::Pannable).empty());
        CHECK(m.IsIdle());
    }

    TEST_CASE("a hold that drifted past the slop never long-presses") {
        GestureMachine m;
        m.OnDown(V(70, 70));
        TickAs(m, kFrame, PressClass::Pannable);
        m.OnMove(V(70, 100));
        TickAs(m, 2 * kFrame, PressClass::Pannable);
        const std::vector<Action> a = TickAs(m, 1.0, PressClass::Pannable);
        CHECK_EQ(Count(a, ActionType::RightDown), 0);
    }

    TEST_CASE("a quick second tap is forwarded at the first tap's point for a double-click") {
        GestureMachine m;
        m.OnDown(V(200, 200));
        TickAs(m, kFrame, PressClass::Pannable);
        m.OnUp(V(200, 200));
        TickAs(m, 2 * kFrame, PressClass::Pannable);

        m.OnDown(V(210, 205));                                        // within the radius, within the time
        CHECK_FALSE(HasButton(TickAs(m, 0.2, PressClass::Pannable))); // decided once hover catches up
        m.OnUp(V(210, 205));                                          // a fast tap: up before the decision
        const std::vector<Action> a = TickAs(m, 0.2 + kFrame, PressClass::Pannable);
        REQUIRE_EQ(a.size(), 3u);
        CHECK_EQ(a[0].type, ActionType::MovePointer);
        CHECK_EQ(a[0].pos.x, 200.0f);
        CHECK_EQ(a[1].type, ActionType::LeftDown);
        CHECK_EQ(a[1].pos.x, 200.0f);
        CHECK_EQ(a[2].type, ActionType::LeftUp);
        CHECK(m.IsIdle());
    }

    TEST_CASE("a quick tap on a neighbouring item is not a double-tap") {
        GestureMachine m;
        m.OnDown(V(200, 200));
        TickAs(m, kFrame, PressClass::Pannable);
        m.OnUp(V(200, 200));
        TickAs(m, 2 * kFrame, PressClass::Pannable);

        m.OnDown(V(200, 221)); // next row: close and quick, but another item
        TickAs(m, 0.1, PressClass::Pannable, nullptr, false);
        m.OnUp(V(200, 221));
        const std::vector<Action> a = TickAs(m, 0.1 + kFrame, PressClass::Pannable, nullptr, false);
        REQUIRE_EQ(a.size(), 2u);
        CHECK_EQ(a[0].type, ActionType::LeftDown);
        CHECK_EQ(a[0].pos.y, 221.0f); // clicks the row actually tapped
        CHECK_EQ(a[1].type, ActionType::LeftUp);
    }

    TEST_CASE("a late or distant second tap is an ordinary tap") {
        GestureMachine m;
        m.OnDown(V(200, 200));
        TickAs(m, kFrame, PressClass::Pannable);
        m.OnUp(V(200, 200));
        TickAs(m, 2 * kFrame, PressClass::Pannable);

        m.OnDown(V(300, 200)); // too far
        CHECK_FALSE(HasButton(TickAs(m, 0.1, PressClass::Pannable)));
        CHECK_EQ(m.State(), GestureState::Pending);
        m.OnUp(V(300, 200));
        TickAs(m, 0.12, PressClass::Pannable);

        m.OnDown(V(300, 200)); // close, but too late after the previous tap
        CHECK_FALSE(HasButton(TickAs(m, 0.9, PressClass::Pannable)));
        CHECK_EQ(m.State(), GestureState::Pending);
    }

    TEST_CASE("duplicate downs during a press are ignored") {
        GestureMachine m;
        m.OnDown(V(10, 10));
        m.OnDown(V(10, 10));
        TickAs(m, kFrame, PressClass::Pannable);
        m.OnUp(V(10, 10));
        const std::vector<Action> a = TickAs(m, 2 * kFrame, PressClass::Pannable);
        CHECK_EQ(Count(a, ActionType::LeftDown), 1);
        CHECK_EQ(Count(a, ActionType::LeftUp), 1);
    }

    TEST_CASE("cancel releases a forwarded press") {
        GestureMachine m;
        m.OnDown(V(10, 10));
        TickAs(m, kFrame, PressClass::DragThrough);
        m.OnMove(V(10, 60));
        TickAs(m, 2 * kFrame, PressClass::DragThrough);
        REQUIRE_EQ(m.State(), GestureState::DragThrough);
        std::vector<Action> out;
        m.Cancel(out);
        REQUIRE_EQ(out.size(), 1u);
        CHECK_EQ(out[0].type, ActionType::LeftUp);
        CHECK(m.IsIdle());
    }

    TEST_CASE("a fast release flings, decays monotonically and stops") {
        GestureMachine m;
        m.OnDown(V(100, 500));
        TickAs(m, 0.0, PressClass::Pannable);
        double t = 0.0;
        float y = 500.0f;
        for (int i = 0; i < 6; ++i) { // 20 px per frame upward = 1200 px/s
            y -= 20.0f;
            t += kFrame;
            m.OnMove(V(100, y));
            TickAs(m, t, PressClass::Pannable);
        }
        m.OnUp(V(100, y - 20.0f));
        t += kFrame;
        TickAs(m, t, PressClass::Pannable);
        REQUIRE_EQ(m.State(), GestureState::Fling);
        CHECK_EQ(m.FlingVelocity().y, doctest::Approx(-1200.0f).epsilon(0.05));

        float lastStep = 1e9f;
        int steps = 0;
        bool ended = false;
        while (steps < 1000 && !ended) {
            t += kFrame;
            const std::vector<Action> a = TickAs(m, t, PressClass::Pannable);
            for (const Action& x : a) {
                if (x.type == ActionType::PanBy) {
                    CHECK_LT(x.delta.y, 0.0f); // keeps moving the way the finger went
                    CHECK_LE(-x.delta.y, lastStep + 1e-3f);
                    lastStep = -x.delta.y;
                }
                if (x.type == ActionType::PanEnd) {
                    ended = true;
                }
            }
            ++steps;
        }
        CHECK(ended);
        CHECK(m.IsIdle());
    }

    TEST_CASE("a slow release or a pause before lifting does not fling") {
        GestureMachine m;
        m.OnDown(V(100, 500));
        TickAs(m, 0.0, PressClass::Pannable);
        m.OnMove(V(100, 400));
        TickAs(m, kFrame, PressClass::Pannable);
        // Rest for longer than the velocity window, then lift.
        TickAs(m, 0.2, PressClass::Pannable);
        TickAs(m, 0.3, PressClass::Pannable);
        m.OnUp(V(100, 400));
        const std::vector<Action> a = TickAs(m, 0.3 + kFrame, PressClass::Pannable);
        CHECK_EQ(Count(a, ActionType::PanEnd), 1);
        CHECK(m.IsIdle());
    }

    TEST_CASE("fling speed is clamped and stops when the scroll target is blocked") {
        GestureMachine m;
        m.OnDown(V(100, 2000));
        TickAs(m, 0.0, PressClass::Pannable);
        m.OnMove(V(100, 1500));
        TickAs(m, 0.01, PressClass::Pannable);
        m.OnUp(V(100, 1000));
        TickAs(m, 0.02, PressClass::Pannable);
        REQUIRE_EQ(m.State(), GestureState::Fling);
        CHECK_LE(Length(m.FlingVelocity()), GestureConfig().flingMaxSpeed + 1.0f);

        m.OnPanAxisBlocked(false);
        const std::vector<Action> a = TickAs(m, 0.04, PressClass::Pannable);
        CHECK_EQ(Count(a, ActionType::PanEnd), 1);
        CHECK(m.IsIdle());
    }

    TEST_CASE("a tap during a fling only stops it") {
        GestureMachine m;
        m.OnDown(V(100, 900));
        TickAs(m, 0.0, PressClass::Pannable);
        m.OnMove(V(100, 800));
        TickAs(m, kFrame, PressClass::Pannable);
        m.OnUp(V(100, 700));
        TickAs(m, 2 * kFrame, PressClass::Pannable);
        REQUIRE_EQ(m.State(), GestureState::Fling);

        m.OnDown(V(100, 300));
        std::vector<Action> a = TickAs(m, 3 * kFrame, PressClass::Pannable);
        CHECK_EQ(Count(a, ActionType::PanEnd), 1);
        m.OnUp(V(100, 300));
        a = TickAs(m, 4 * kFrame, PressClass::Pannable);
        CHECK_FALSE(HasButton(a));
        CHECK(m.IsIdle());
    }

    TEST_CASE("fling decay does not depend on the frame rate") {
        Vec2 v60 = V(0, 1000);
        Vec2 v30 = V(0, 1000);
        float d60 = 0.0f;
        float d30 = 0.0f;
        for (int i = 0; i < 60; ++i) {
            d60 += FlingStep(v60, 1.0f / 60.0f, 0.325f).y;
        }
        for (int i = 0; i < 30; ++i) {
            d30 += FlingStep(v30, 1.0f / 30.0f, 0.325f).y;
        }
        CHECK_EQ(d60, doctest::Approx(d30).epsilon(0.001));
        CHECK_EQ(v60.y, doctest::Approx(v30.y).epsilon(0.001));
    }

    TEST_CASE("config scaling and long-press clamping") {
        const GestureConfig c = ScaledBy(GestureConfig(), 2.0f);
        CHECK_EQ(c.tapSlopPx, doctest::Approx(20.0f));
        CHECK_EQ(c.flingMinSpeed, doctest::Approx(240.0f));
        CHECK_EQ(c.longPressSeconds, doctest::Approx(kDefaultLongPressSeconds));
        CHECK_EQ(ClampLongPressSeconds(0.0f), doctest::Approx(kMinLongPressSeconds));
        CHECK_EQ(ClampLongPressSeconds(9.0f), doctest::Approx(kMaxLongPressSeconds));
        CHECK_EQ(ClampLongPressSeconds(0.8f), doctest::Approx(0.8f));
    }
}
