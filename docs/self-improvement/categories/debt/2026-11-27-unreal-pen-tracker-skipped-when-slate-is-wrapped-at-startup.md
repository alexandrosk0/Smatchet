# The Unreal pen tracker is skipped when Slate's platform application is wrapped at startup

- **Category**: debt
- **Priority**: P3
- **Date**: 2026-11-27
- **Observed on**: gate review of the #2500 fix (`SmatchetImGuiInputProcessor.cpp`, the processor constructor).
- **Status**: open

## What happened

The #2500 fix registers the pen-pointer message handler only when Slate's platform application is the native `FWindowsApplication` (identity check through `GetInputInterface()`). If a Pixel Streaming application wrapper or the automation driver's `FAutomatedApplication` is already installed when the processor is built, registration is skipped, one Log line says so, and pen contacts are handled as touch for the session. On a normal launch the processor is built in `StartupModule` before either can swap the application in, so this only follows a module restart during active remote input or automation.

The native application is still reachable in that case: the wrappers forward `GetInputInterface()` to the application they wrap, and `FWindowsApplication::AddMessageHandler` / `RemoveMessageHandler` act on the file-global native application (UE 5.7 `WindowsApplication.cpp:4393-4402`).

## Concrete next action

- If pen input under Pixel Streaming or automation matters, register through the wrapped application: when the identity check fails, take `GetInputInterface()`, apply the same identity test to the object it returns, and remember that object (not the wrapper) in `GSmatchetPenTrackerApp`. Verify in a UBT build plus a manual run with Pixel Streaming 2 enabled.
