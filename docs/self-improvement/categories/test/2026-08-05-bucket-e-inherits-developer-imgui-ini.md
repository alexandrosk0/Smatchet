# Bucket-E drivers boot against the developer's real `%LOCALAPPDATA%\Smatchet\imgui.ini`

- **Category**: test
- **Priority**: P1
- **Date**: 2026-08-05
- **Status**: open (one driver fixed; the rest unaudited)

## What happened

`scripts/dev/test-ui-window-expand.sh` failed `TabBarToggleClickExpandsThenMinimizes`
deterministically on one machine and passed on another. The geometry assertion
(`RectFull.Max.x == BarRect.Max.x`) passed; only `ItemClick` on the docked toggle
could not land:

```
Failed to move window 'Views - Jira###SmatchetViewsDashboard'! While trying to make
space to click at (1245.50,102.50) over window 'WindowOverViewport_.../DockSpace_...'.
Error 'MouseMove: Unable to Hover 0xED75AA4E ... Hovered id was 0x00000000 in ''.
```

Root cause: the spawned app resolves its user-data dir via
`ConfigManager::GetPlatformSharedUserDataDirectory()` → `%LOCALAPPDATA%\Smatchet`,
so it loads whatever `imgui.ini` the developer's own interactive session last
wrote. Windows the test never opens were left floating over the dock tab bar,
making the docked control unhoverable. Pointing `LOCALAPPDATA` at an empty
throwaway dir took the same binary from 6/7 to **7/7**.

A first fix that assumed z-order (park overlapping floaters, then click) was
written, built, and re-run — it changed nothing, because the offender was itself
docked. Worth recording: the *hypothesis* was wrong, and only swapping the
environment proved which layer owned the failure.

## Why it matters

Any bucket-E (and bucket-C screenshot) lane that boots the real exe inherits
per-developer, per-boot dock state. That is a silent correctness hole in the test
bucket that is supposed to keep visual features out of the human-verification
pause: a green local run is not evidence the layout under test was the shipped
default, and a red one is not evidence of a product defect.

## Proposed fix

1. Audit every driver that spawns the exe (`scripts/dev/test-ui-*.sh`,
   `test-screenshot-diff.sh`) for the same exposure.
2. Factor the isolation into one shared helper rather than the per-driver
   `mktemp -d` + `export LOCALAPPDATA/APPDATA/XDG_CONFIG_HOME` + `trap` block now
   in `test-ui-window-expand.sh`.
3. Consider a first-party knob instead of environment shadowing —
   `ConfigManager::GetPlatformSharedOverrideRef()` already exists as a
   programmatic hook and would not depend on the platform's env-var names.

## Evidence

- Same binary, same host: real `LOCALAPPDATA` → `Passed: 6 Failed: 1`;
  throwaway dir → `Passed: 7 Failed: 0`.
- Resolution order read from `Source/Core/src/Config/ConfigManager_PathUtils.cpp`
  (`GetPlatformSharedUserDataDirectory`, `GetImGuiSettingsPath`).
