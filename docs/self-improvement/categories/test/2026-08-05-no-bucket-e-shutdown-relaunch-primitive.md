# No bucket-E primitive can assert what the app persisted to `imgui.ini` at shutdown

- **Category**: test
- **Priority**: P1
- **Date**: 2026-08-05
- **Status**: open

## What happened

The window-expand feature (`Source/Core/src/Ui/SmatchetWindowExpand.cpp`) overrides a
window's dock id and geometry per frame. That override IS the window's live state, and
`WindowSettingsHandler_WriteAll` snapshots `Pos`/`Size`/`DockId` off the live window —
emitting the `DockId=` line only `if (settings->DockId != 0)`. So the `io.IniSavingRate`
auto-save, and the unconditional save in `DestroyContext`, persist an expanded window as
floating + fullscreen. On relaunch `SmatchetUI_Layout`'s `pendingReDockWindows` path
force-redocks it within ~2 frames — to its **default** slot, so a customised dock placement
is lost.

The code review that caught this also confirmed the gap: **no test bucket can observe it.**
Bucket A is ImGui-free, bucket E runs inside one process lifetime, bucket C compares frames
of a single boot. The behaviour is documented in the header instead, and the fix deferred,
because untested shutdown-time code that rewrites `imgui.ini` is riskier than the
self-recovering limitation.

## Why it matters

"State the app wrote at exit" is invisible to every gate we have. Any feature that overrides
window geometry, dock ids or visibility per frame inherits the same blind spot, and the only
signal is a user noticing their layout drifted after a restart — exactly the class of bug
the bucket-E investment was meant to keep out of human verification.

## Proposed fix

1. A bucket-E harness primitive: boot the exe against an isolated user-data dir (see
   [`2026-08-05-bucket-e-inherits-developer-imgui-ini.md`](2026-08-05-bucket-e-inherits-developer-imgui-ini.md)),
   drive it, shut it down cleanly, then **relaunch against the `imgui.ini` it produced** and
   assert the restored layout. Route to `test-author`.
2. With that in place, either make the expand transition ini-safe (write the pre-expand
   placement into the window's settings entry rather than leaving the override live), or
   drop the expansion during shutdown so the saved state is the home slot.
3. A line in `Source/Core/src/Ui/AGENTS.md`: any per-frame override of window
   geometry / dock id / visibility is persisted verbatim by ImGui's settings writer — say
   what the app writes at exit, or make the override not be the live state.
