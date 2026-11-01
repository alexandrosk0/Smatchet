# Touchscreen gestures have no automated check on a real touch input path

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-10-31
- **Observed on**: the touchscreen-support work (`docs/plans/touchscreen-support.md`).
- **Status**: open

## What happened

The gesture router (`Source/Core/src/Ui/TouchInputRouter.cpp`) is covered by unit tests (`tests/Core/TouchGesturePure.test.cpp`) and a bucket-E suite (`tests/ui/touch_gestures.test.cpp`). The bucket-E suite injects finger input straight into the router, the same call the hosts make. Two host paths in front of that call are not exercised by anything automated:

1. **Standalone.** `StandaloneTouchBridge.cpp` decides "touch or mouse" from the GLFW backend's source latch. The backend sets that latch from `GetMessageExtraInfo()` in its WndProc hook. The bridge also sets `MicrosoftTabletPenServiceProperty` to switch off Windows' own press-and-hold right click. Neither is checked without a real touchscreen.
2. **Unreal.** `SmatchetImGuiInputProcessor.cpp` reports `FPointerEvent::IsTouchEvent()` through `SmatchetHost_SetMouseSource`. The engine source shows the route (`FSlateApplication::OnTouchStarted` → `ProcessTouchStartedEvent` → `ProcessMouseButtonDownEvent` → `InputPreProcessors.HandleMouseButtonDownEvent`; touch `FPointerEvent`s set `bIsTouchEvent`, UE 5.7 `SlateApplication.cpp` / `Events.h`), but no test drives it.

## Concrete next action

- **Standalone:** add a Windows-only bucket-E driver that calls `InjectSyntheticPointerInput` (PT_TOUCH, Windows 10 1809+) against the app window. Assert through the existing `ui_test.run` probes that a tap clicks, a hold opens a context menu, and a drag scrolls. That covers the WndProc latch, the bridge and the tablet property end to end.
- **Unreal:** add a UE Automation spec that calls `FSlateApplication::Get().OnTouchStarted / OnTouchMoved / OnTouchEnded` and asserts host state after `BeginFrame` (for example a new `SmatchetHost_GetLastMouseSource` probe plus `io.MouseDown[0]`).

Until then both are manual checks on a touchscreen device.
