# The minimized-window frame throttle has no automated CPU check

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-02
- **Observed on**: background-traffic idle gating (`docs/plans/background-traffic-idle-gating.md`).
- **Status**: open

## What happened

The standalone frame loop now waits for events at about 30 Hz while the window is minimized (`PumpPlatformEvents` in `Source/Standalone/main.cpp`), instead of polling at full speed with nothing to present. The waiting branch is only taken when `GLFW_ICONIFIED` is set, and nothing automated ever minimizes the window, so the CPU drop is a by-hand Task Manager check. The rest of the change (activity hub, probe gating, backoff) is covered by `tests/Core/ConnectivityMonitorService.test.cpp`.

## Concrete next action

The throttle lives on the GUI window path (`RunFrameLoop` in `Source/Standalone/main.cpp`), not the headless `--spawn` loop in `StandaloneAppBootstrap.cpp`, so the probe must drive the real window. Launch the GUI exe with `SMATCHET_AUTORUN_SCENARIO` pointing at a new scenario (or drive it over MCP) that calls `glfwIconifyWindow` (a new `window.minimize`-style debug command), have the bash driver sample the process CPU time over ~5 s (`GetProcessTimes` / `ps`), restore the window, and assert the minimized-phase CPU share stays under a fixed budget (for example < 10 % of one core).
