#include <doctest/doctest.h>

#include "Ui/StatusBarAutoHidePure.h"

using namespace StatusBarAutoHidePure;

TEST_SUITE("StatusBarAutoHidePure") {
    TEST_CASE("ModeFromConfig") {
        // !show => Hidden
        CHECK_EQ(ModeFromConfig(false, false), Mode::Hidden);
        CHECK_EQ(ModeFromConfig(false, true), Mode::Hidden);

        // show && !autoHide => Always
        CHECK_EQ(ModeFromConfig(true, false), Mode::Always);

        // show && autoHide => AutoHide
        CHECK_EQ(ModeFromConfig(true, true), Mode::AutoHide);
    }

    TEST_CASE("first frame doesn't flash") {
        State state;
        Signals sig{true, 0, false, 0}; // has problem
        bool visible = Tick(state, sig, 0.0, false, false);
        CHECK_FALSE(state.flashUntil > 0);
        CHECK(visible); // needs attention
    }

    TEST_CASE("change in signals flashes") {
        State state;
        Signals sig1{false, 0, false, 0}; // no attention
        Signals sig2{true, 0, false, 0};  // has problem

        // First frame, no flash.
        Tick(state, sig1, 0.0, false, false);
        CHECK_EQ(state.flashUntil, 0);

        // Change at t=1, flash starts.
        Tick(state, sig2, 1.0, false, false);
        CHECK_EQ(state.flashUntil, 1.0 + kFlashSeconds);
    }

    TEST_CASE("flash expires") {
        State state;
        Signals sig1{false, 0, false, 0};
        Signals sig2{true, 0, false, 0};

        Tick(state, sig1, 0.0, false, false);
        Tick(state, sig2, 1.0, false, false);

        // During flash, visible.
        CHECK(Tick(state, sig1, 2.0, false, false));

        // After flash expires, hidden (no other attention).
        CHECK_FALSE(Tick(state, sig1, 1.0 + kFlashSeconds + 0.1, false, false));
    }

    TEST_CASE("attention keeps bar visible") {
        State state;
        Signals sig{true, 0, false, 0};

        Tick(state, sig, 0.0, false, false);
        CHECK(Tick(state, sig, 100.0, false, false)); // far in future, still visible
    }

    TEST_CASE("Unknown connectivity doesn't pin the bar") {
        // Only auth/config error, transport down, service unavailable count as problems.
        // Unknown state should not block the bar.
        State state;
        Signals sig{false, 0, false, 0}; // no problem, no queued ops, no in-flight, no errors

        Tick(state, sig, 0.0, false, false);
        CHECK_FALSE(Tick(state, sig, 1.0, false, false)); // bar hides
    }

    TEST_CASE("queued ops keep bar visible") {
        State state;
        Signals sig{false, 1, false, 0}; // 1 queued op

        Tick(state, sig, 0.0, false, false);
        CHECK(Tick(state, sig, 100.0, false, false));
    }

    TEST_CASE("in-flight edit keeps bar visible") {
        State state;
        Signals sig{false, 0, true, 0}; // saving

        Tick(state, sig, 0.0, false, false);
        CHECK(Tick(state, sig, 100.0, false, false));
    }

    TEST_CASE("unread errors keep bar visible") {
        State state;
        Signals sig{false, 0, false, 3}; // 3 errors

        Tick(state, sig, 0.0, false, false);
        CHECK(Tick(state, sig, 100.0, false, false));
    }

    TEST_CASE("hover dwell requires time in zone") {
        State state;
        Signals sig{false, 0, false, 0}; // no attention

        Tick(state, sig, 0.0, false, false);
        CHECK_FALSE(Tick(state, sig, 0.1, true, false)); // in zone but too fast

        // Enough dwell time (0.1 + 0.25 = 0.35, use 0.36 to avoid floating-point boundary).
        CHECK(Tick(state, sig, 0.36, true, false));
    }

    TEST_CASE("mouse button blocks pointer reveal") {
        State state;
        Signals sig{false, 0, false, 0};

        Tick(state, sig, 0.0, false, false);

        // Button down while in zone: dwell doesn't start.
        Tick(state, sig, 0.1, true, true);
        CHECK_EQ(state.hoverSince, -1);

        // Release button and wait: dwell starts anew.
        Tick(state, sig, 0.2, true, false);
        CHECK_GE(state.hoverSince, 0);
    }

    TEST_CASE("button down doesn't hide a visible bar") {
        State state;
        Signals sig{false, 0, false, 0};

        Tick(state, sig, 0.0, false, false);
        // Enter zone at t=0.01, then wait until dwell time is met.
        Tick(state, sig, 0.01, true, false);
        Tick(state, sig, 0.26, true, false);
        CHECK(state.visible);

        // Button down: bar stays visible.
        bool stillVisible = Tick(state, sig, 0.27, true, true);
        CHECK(stillVisible);
    }

    TEST_CASE("grace period after pointer leaves") {
        State state;
        Signals sig{false, 0, false, 0};

        Tick(state, sig, 0.0, false, false);
        // Enter zone and establish dwell (enter at t=0.01, dwell at t=0.26+).
        Tick(state, sig, 0.01, true, false);
        Tick(state, sig, 0.26, true, false);
        CHECK(state.visible);

        // Leave zone.
        Tick(state, sig, 0.27, false, false);
        CHECK(state.hoverLeftAt >= 0);
        CHECK(state.visible); // grace is active

        // Grace expires (need t > 0.27 + 0.4 = 0.67 for grace to expire).
        CHECK_FALSE(Tick(state, sig, 0.68, false, false));
    }

    TEST_CASE("pointer re-entering resets dwell") {
        State state;
        Signals sig{false, 0, false, 0};

        Tick(state, sig, 0.0, false, false);
        // Enter zone but not long enough.
        Tick(state, sig, 0.1, true, false);
        double firstHoverSince = state.hoverSince;

        // Leave.
        Tick(state, sig, 0.15, false, false);
        CHECK_EQ(state.hoverSince, -1);

        // Re-enter.
        Tick(state, sig, 0.2, true, false);
        CHECK_GT(state.hoverSince, firstHoverSince); // timer restarted
    }

    TEST_CASE("PointerInZone: hidden bar, no inset") {
        // Hidden bar, no grip inset: zone is kRevealZonePx above bottom.
        const float workBottom = 1000.0f;
        const float barH = 24.0f;
        const float inset = 0.0f;

        // Just below threshold: not in zone.
        CHECK_FALSE(PointerInZone(workBottom - kRevealZonePx + 1, workBottom, inset, barH, false));

        // At threshold: in zone.
        CHECK(PointerInZone(workBottom - kRevealZonePx, workBottom, inset, barH, false));

        // Well above: in zone.
        CHECK(PointerInZone(workBottom - kRevealZonePx - 10, workBottom, inset, barH, false));
    }

    TEST_CASE("PointerInZone: hidden bar with grip inset") {
        // Hidden bar, grip inset > kRevealZonePx: zone is inset height.
        const float workBottom = 1000.0f;
        const float barH = 24.0f;
        const float gripInset = 10.0f;

        // Below grip inset: not in zone.
        CHECK_FALSE(PointerInZone(workBottom - gripInset + 1, workBottom, gripInset, barH, false));

        // At grip inset: in zone.
        CHECK(PointerInZone(workBottom - gripInset, workBottom, gripInset, barH, false));
    }

    TEST_CASE("PointerInZone: visible bar") {
        // Visible bar: zone is immediately above the bar position.
        const float workBottom = 1000.0f;
        const float barH = 24.0f;
        const float inset = 7.0f;

        const float barTop = workBottom - inset - barH;

        // Below bar top: not in zone.
        CHECK_FALSE(PointerInZone(barTop - 1, workBottom, inset, barH, true));

        // At bar top: in zone.
        CHECK(PointerInZone(barTop, workBottom, inset, barH, true));

        // Inside bar: in zone.
        CHECK(PointerInZone(barTop + 5, workBottom, inset, barH, true));
    }

    TEST_CASE("Signals equality") {
        Signals sig1{true, 5, false, 2};
        Signals sig2{true, 5, false, 2};
        Signals sig3{false, 5, false, 2};

        CHECK_EQ(sig1, sig2);
        CHECK_NE(sig1, sig3);
    }

    TEST_CASE("NeedsAttention") {
        CHECK(NeedsAttention({true, 0, false, 0}));        // problem
        CHECK(NeedsAttention({false, 1, false, 0}));       // queued
        CHECK(NeedsAttention({false, 0, true, 0}));        // saving
        CHECK(NeedsAttention({false, 0, false, 1}));       // errors
        CHECK_FALSE(NeedsAttention({false, 0, false, 0})); // nothing
    }
}
