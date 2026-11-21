#pragma once

// The `preferences.search.focus` command's hand-off to the Preferences window: the command opens and
// raises the window, then asks for the caret in its settings search box. Defined in
// SmatchetPreferencesUi.cpp, which owns the request: the next Preferences draw no popup holds the
// keyboard takes it, and a request no draw took within a few frames expires (the window did not
// come up) instead of pulling the caret in later, unasked. UI thread only.

namespace smatchet {
namespace ui {

void RequestPrefsSearchFocus();

#if defined(SMATCHET_BUILD_UI_TESTS)
/// Bucket-E seam: how many times Enter in the settings search sent the keyboard to the first matching
/// setting. A test cannot rely on where ImGui's navigation lands, which depends on how the test drives
/// window focus; this says the decision itself was made.
int PrefsSearchFirstMatchFocusCountForTest();
#endif

} // namespace ui
} // namespace smatchet
