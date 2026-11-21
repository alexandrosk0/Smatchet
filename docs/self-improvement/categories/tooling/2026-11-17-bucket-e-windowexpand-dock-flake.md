# Bucket-E headless lane: two WindowExpand dock-node cases fail intermittently on identical code

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-17
- **Observed on**: PR #2440, `Bucket-E UI tests (Mesa headless GL)`, workflow run 44003734158, head `4adb75f`. First attempt 249 of 251; a re-run of the failed job on the same head passed. The previous head, `933b940`, has the same code and passed 251 of 251.
- **Status**: open

## What happened

Two cases failed on the first attempt, both on a dock-node or window-sweep precondition:

- `WindowExpand/SplitterDragAfterMinimizeKeepsDocking`, `window_expand.test.cpp:708`, `'swept'` (the helper that sweeps the splitter did not report a sweep).
- `WindowExpand/ScriptingWindowContributesTabBarToggle`, `window_expand.test.cpp:792`, `'win != nullptr && win->DockNode != nullptr'`.

Neither case touches the piece-4.6 code. `SmatchetRegisterWindowExpandTests` is registered before `SmatchetRegisterGestureKeysTests` in `tests/ui/ui_tests_registry.cpp`, so the gesture-key cleanup cannot have leaked state into them, and every `GestureKeys` case passed in the same run. The identical code passed twice (the earlier head, and the re-run), so the cause is timing or dock realisation in the headless software-GL lane, the same property that stops the panel-resize case from running there (see `2026-11-17-bucket-e-headless-lane-realises-no-edge-panel`).

A first-attempt failure on a code-identical head makes the lane's per-test verdicts, which are blocking, cost a manual re-run each time it recurs.

## Concrete next action

- Count recurrences: if either case fails again on a head that did not touch docking, give the two cases the same headless precondition skip `reset_layout_docking.test.cpp` uses (a logged skip when no dock node is realised), or make them wait on dock realisation with a bounded `YieldUntil`.
- Do not widen the skip beyond the dock-node precondition: the cases still guard real regressions on a real-GL run.

Last-reviewed: 2026-11-17
