#pragma once

#include "Ui/GestureKeysPure.h"

/// The UI-thread instance of the gesture-key latch (Ui/GestureKeysPure.h). The ui.list.* / ui.splitter.*
/// commands post into it; a list or splitter takes a request only while it has the keyboard. Every function
/// here must run on the UI thread inside a frame (it reads ImGui's frame count and focus).
namespace smatchet {
namespace ui {

/// Posts a request for `key`, stamped with the current frame.
void PostGestureKey(GestureKey key);

/// Takes a live request for `key` when `targetHasFocus`. With no focus the request is left for the widget
/// that does have it, so the call never consumes a key meant for another list or splitter.
bool TakeGestureKey(GestureKey key, bool targetHasFocus);

/// TakeGestureKey with the focus test for the current window: it, or a child of it, has the keyboard.
bool TakeGestureKeyForCurrentWindow(GestureKey key);

} // namespace ui
} // namespace smatchet
