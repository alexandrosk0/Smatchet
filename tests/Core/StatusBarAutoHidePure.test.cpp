// Pure-logic coverage for the status-bar Auto-Hide state machine (StatusBarAutoHidePure.h).
// Pins the contract the impure driver (DrawStatusBarAutoHide) feeds live signals + pointer
// geometry into: attention states pin the bar, any signal change flashes it, the bottom-edge
// hover reveal needs a dwell and never starts under a held button, and the reveal zone is a
// thin band at the bottom (lifted above the bottom-panel reveal grip when it is present).

#include "Ui/StatusBarAutoHidePure.h"

#include <doctest/doctest.h>

using namespace StatusBarAutoHidePure;

namespace {

const Signals kIdle{false, 0, false, 0};
const Signals kOffline{true, 0, false, 0};

// Drives the pointer into the zone long enough to reveal the bar; returns the time it showed.
double RevealByHover(State& state, const Signals& sig, double enterT) {
    Tick(state, sig, enterT, true, false);
    const double shownT = enterT + kHoverDwellSeconds + 0.01;
    REQUIRE(Tick(state, sig, shownT, true, false));
    return shownT;
}

} // namespace

TEST_SUITE("StatusBarAutoHidePure") {
    TEST_CASE("ModeFromConfig maps the two config flags to three modes") {
        CHECK_EQ(ModeFromConfig(false, false), Mode::Hidden);
        CHECK_EQ(ModeFromConfig(false, true), Mode::Hidden);
        CHECK_EQ(ModeFromConfig(true, false), Mode::Always);
        CHECK_EQ(ModeFromConfig(true, true), Mode::AutoHide);
    }

    TEST_CASE("first frame records the baseline without flashing") {
        State idle;
        CHECK_FALSE(Tick(idle, kIdle, 0.0, false, false));
        CHECK_EQ(idle.flashUntil, 0.0);

        State offline;
        CHECK(Tick(offline, kOffline, 0.0, false, false)); // visible for attention, not a flash
        CHECK_EQ(offline.flashUntil, 0.0);
    }

    TEST_CASE("entering an attention state arms a flash") {
        State state;
        Tick(state, kIdle, 0.0, false, false);
        CHECK(Tick(state, kOffline, 1.0, false, false));
        CHECK_EQ(state.flashUntil, doctest::Approx(1.0 + kFlashSeconds));
    }

    TEST_CASE("recovering from attention flashes, then the bar hides") {
        State state;
        Tick(state, kOffline, 0.0, false, false);
        CHECK(Tick(state, kOffline, 5.0, false, false));
        // offline -> online at t=6: no attention any more, but the change is shown.
        CHECK(Tick(state, kIdle, 6.0, false, false));
        CHECK(Tick(state, kIdle, 6.0 + kFlashSeconds - 0.1, false, false));
        CHECK_FALSE(Tick(state, kIdle, 6.0 + kFlashSeconds + 0.1, false, false));
    }

    TEST_CASE("any counter change flashes, including a decrease") {
        State state;
        Tick(state, Signals{false, 2, false, 0}, 0.0, false, false);
        CHECK(Tick(state, kIdle, 1.0, false, false)); // queue drained
        CHECK_EQ(state.flashUntil, doctest::Approx(1.0 + kFlashSeconds));
    }

    TEST_CASE("each attention signal pins the bar") {
        const Signals pinned[] = {kOffline, Signals{false, 1, false, 0}, Signals{false, 0, true, 0},
                                  Signals{false, 0, false, 3}};
        for (const Signals& sig : pinned) {
            State state;
            Tick(state, sig, 0.0, false, false);
            CHECK(Tick(state, sig, 100.0, false, false));
        }
    }

    TEST_CASE("idle signals keep the bar hidden") {
        // Unknown connectivity maps to trackerProblem=false, so a setup with no tracker does
        // not pin the bar.
        State state;
        Tick(state, kIdle, 0.0, false, false);
        CHECK_FALSE(Tick(state, kIdle, 1.0, false, false));
    }

    TEST_CASE("hover reveal needs the dwell time") {
        State state;
        Tick(state, kIdle, 0.0, false, false);
        CHECK_FALSE(Tick(state, kIdle, 0.10, true, false));
        CHECK_FALSE(Tick(state, kIdle, 0.10 + kHoverDwellSeconds - 0.01, true, false));
        CHECK(Tick(state, kIdle, 0.10 + kHoverDwellSeconds + 0.01, true, false));
    }

    TEST_CASE("a held button blocks starting the dwell") {
        State state;
        Tick(state, kIdle, 0.0, false, false);
        // Dragging along the bottom edge (scrollbar / splitter): never reveals.
        CHECK_FALSE(Tick(state, kIdle, 0.1, true, true));
        CHECK_FALSE(Tick(state, kIdle, 1.0, true, true));
        CHECK_EQ(state.hoverSince, -1.0);
        // Released: the dwell starts from the release.
        CHECK_FALSE(Tick(state, kIdle, 1.1, true, false));
        CHECK(Tick(state, kIdle, 1.1 + kHoverDwellSeconds + 0.01, true, false));
    }

    TEST_CASE("a held button never hides a bar that is already visible") {
        State state;
        Tick(state, kIdle, 0.0, false, false);
        const double shownT = RevealByHover(state, kIdle, 0.01);
        CHECK(Tick(state, kIdle, shownT + 0.01, true, true));
        CHECK(Tick(state, kIdle, shownT + 2.0, true, true)); // held for a long press
    }

    TEST_CASE("hovering a bar shown for attention keeps it after the attention clears") {
        State state;
        Tick(state, kOffline, 0.0, false, false);
        CHECK(Tick(state, kOffline, 1.0, true, true)); // pointer on the bar, button down
        // Attention clears; flash runs out; pointer still on the bar keeps it open.
        Tick(state, kIdle, 2.0, true, false);
        CHECK(Tick(state, kIdle, 2.0 + kFlashSeconds + 1.0, true, false));
    }

    TEST_CASE("grace period after the pointer leaves a revealed bar") {
        State state;
        Tick(state, kIdle, 0.0, false, false);
        const double shownT = RevealByHover(state, kIdle, 0.01);
        const double leftT = shownT + 0.01;
        CHECK(Tick(state, kIdle, leftT, false, false));
        CHECK(Tick(state, kIdle, leftT + kHoverGraceSeconds - 0.05, false, false));
        CHECK_FALSE(Tick(state, kIdle, leftT + kHoverGraceSeconds + 0.05, false, false));
    }

    TEST_CASE("passing over the edge before the dwell never flickers the bar") {
        State state;
        Tick(state, kIdle, 0.0, false, false);
        CHECK_FALSE(Tick(state, kIdle, 0.10, true, false));
        CHECK_FALSE(Tick(state, kIdle, 0.15, false, false));
        CHECK_EQ(state.hoverLeftAt, -1.0);
        CHECK_FALSE(Tick(state, kIdle, 0.20, false, false));
    }

    TEST_CASE("re-entering the zone restarts the dwell") {
        State state;
        Tick(state, kIdle, 0.0, false, false);
        Tick(state, kIdle, 0.10, true, false);
        Tick(state, kIdle, 0.15, false, false);
        Tick(state, kIdle, 0.20, true, false);
        CHECK_EQ(state.hoverSince, doctest::Approx(0.20));
        CHECK_FALSE(Tick(state, kIdle, 0.10 + kHoverDwellSeconds + 0.01, true, false));
    }

    TEST_CASE("PointerInZone: hidden bar, no grip - only the bottom band") {
        const float bottom = 1000.0f;
        const float barH = 24.0f;
        CHECK(PointerInZone(bottom - 1.0f, bottom, 0.0f, barH, false));
        CHECK(PointerInZone(bottom - kRevealZonePx, bottom, 0.0f, barH, false));
        CHECK_FALSE(PointerInZone(bottom - kRevealZonePx - 1.0f, bottom, 0.0f, barH, false));
        CHECK_FALSE(PointerInZone(bottom - 500.0f, bottom, 0.0f, barH, false)); // mid-screen
    }

    TEST_CASE("PointerInZone: hidden bar with the reveal grip - the grip band") {
        const float bottom = 1000.0f;
        const float barH = 24.0f;
        const float grip = 7.0f;
        CHECK(PointerInZone(bottom - grip, bottom, grip, barH, false));
        CHECK_FALSE(PointerInZone(bottom - grip - 1.0f, bottom, grip, barH, false));
        // An inset thinner than the default band never shrinks the zone.
        CHECK(PointerInZone(bottom - kRevealZonePx, bottom, 2.0f, barH, false));
    }

    TEST_CASE("PointerInZone: visible bar - the bar plus the grip below it") {
        const float bottom = 1000.0f;
        const float barH = 24.0f;
        const float grip = 7.0f;
        const float barTop = bottom - grip - barH;
        CHECK(PointerInZone(barTop, bottom, grip, barH, true));
        CHECK(PointerInZone(barTop + 5.0f, bottom, grip, barH, true));
        CHECK(PointerInZone(bottom - 1.0f, bottom, grip, barH, true));
        CHECK_FALSE(PointerInZone(barTop - 1.0f, bottom, grip, barH, true));
    }

    TEST_CASE("Signals equality and NeedsAttention") {
        CHECK(Signals{true, 5, false, 2} == Signals{true, 5, false, 2});
        CHECK(Signals{true, 5, false, 2} != Signals{false, 5, false, 2});
        CHECK_FALSE(NeedsAttention(kIdle));
        CHECK(NeedsAttention(kOffline));
    }
}
