#pragma once

// AppActivity — tiny GLFW/ImGui-free hub recording whether the user is still around. Each host
// reports one sample per frame: whether its window is focused and whether ImGui processed any
// input event that frame. Command dispatch (palette, MCP, CLI, Lua) also counts as activity, so an
// agent driving an unfocused window keeps it awake. Background work that only matters to an
// attending user (the tracker connectivity probe) asks IsBackgroundIdle() before going to the
// network. Header-only atomics so it compiles into SmatchetCore_DX12 and the headless TSan subset
// (same shape as VsyncControl.h).
//
// Background-idle = a host has reported at least once AND the window is unfocused AND the last
// focused, input, or command activity is older than kBackgroundIdleAfter. A host that never
// reports (Android, which already stops ticking when unfocused; headless CLI) is never idle. An
// external client that keeps dispatching commands (an MCP statusline, a Blueprint timer) keeps the
// app awake by design: someone is driving it.
// Plan: docs/plans/background-traffic-idle-gating.md

#include <atomic>
#include <chrono>
#include <cstdint>

namespace smatchet {
namespace activity {

constexpr std::chrono::minutes kBackgroundIdleAfter{5};

/// Pure predicate behind IsBackgroundIdle (unit-testable without the global hub).
inline bool IsBackgroundIdleAt(bool reported, bool focused, std::chrono::steady_clock::time_point lastActive,
                               std::chrono::steady_clock::time_point now) {
    return reported && !focused && now - lastActive >= kBackgroundIdleAfter;
}

struct HubState {
    std::atomic<bool> Reported{false};
    std::atomic<bool> Focused{true};
    std::atomic<std::int64_t> LastActiveTicks{0}; // steady_clock::duration count since epoch
};

inline HubState& State() {
    static HubState state;
    return state;
}

/// One sample per frame from the host. Any thread; plain relaxed atomics (a frame of skew is fine).
inline void NoteFrame(bool focused, bool hadInput, std::chrono::steady_clock::time_point now) {
    HubState& s = State();
    s.Focused.store(focused, std::memory_order_relaxed);
    if (focused || hadInput || !s.Reported.load(std::memory_order_relaxed)) {
        s.LastActiveTicks.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    }
    s.Reported.store(true, std::memory_order_relaxed);
}

/// Activity that does not come through the host's input path (a dispatched command). Any thread;
/// leaves the focus sample alone.
inline void NoteActivity(std::chrono::steady_clock::time_point now) {
    State().LastActiveTicks.store(now.time_since_epoch().count(), std::memory_order_relaxed);
}

inline bool IsBackgroundIdle(std::chrono::steady_clock::time_point now) {
    const HubState& s = State();
    const std::chrono::steady_clock::time_point lastActive(
        std::chrono::steady_clock::duration(s.LastActiveTicks.load(std::memory_order_relaxed)));
    return IsBackgroundIdleAt(s.Reported.load(std::memory_order_relaxed), s.Focused.load(std::memory_order_relaxed),
                              lastActive, now);
}

/// Back to the never-reported state (tests only — the hub is process-global).
inline void ResetForTest() {
    HubState& s = State();
    s.Reported.store(false, std::memory_order_relaxed);
    s.Focused.store(true, std::memory_order_relaxed);
    s.LastActiveTicks.store(0, std::memory_order_relaxed);
}

} // namespace activity
} // namespace smatchet
