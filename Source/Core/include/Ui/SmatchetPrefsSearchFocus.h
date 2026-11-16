#pragma once

// The `preferences.search.focus` command's hand-off to the Preferences window: the command opens and
// raises the window, then asks for the caret in its settings search box. Defined in
// SmatchetPreferencesUi.cpp, which owns the request: the next Preferences draw no popup holds the
// keyboard takes it, and a request no draw took within a few frames expires (the window did not
// come up) instead of pulling the caret in later, unasked. UI thread only.

namespace smatchet {
namespace ui {

void RequestPrefsSearchFocus();

} // namespace ui
} // namespace smatchet
