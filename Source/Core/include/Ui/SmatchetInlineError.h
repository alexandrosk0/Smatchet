#pragma once

// The one place an input field's parse problem (or its parsed preview) is drawn, so every editor says it
// the same way: a red line under the field while the user types, naming the part that is wrong and how to
// fix it (input UX audit, section 4.5). The parse layer's describe function supplies the text, so it is
// already localized.
//
// Draw order: call right after the widget it describes, in the same window, so the line sits under it.
// Nothing here owns state beyond the test seam, and nothing takes focus: the line is not a window.

#include "Tracker/TrackerFieldValueParseExplain.h"

#include "imgui.h"

#include <string>

namespace smatchet {
namespace ui {

/// A red wrapped line in the current layout, under the item just drawn (a dialog or form field). Takes the
/// room it needs. Does nothing for a message with no error.
void DrawInlineParseError(const TrackerParseExplain::ParseMessage& msg);

/// The same line as a dim "preview" when the text parses but is not yet in its final form, such as
/// "= 1h 30m" under "90 m". Does nothing for empty text.
void DrawInlineParseHint(const std::string& text);

/// The red line painted on the foreground just under the last item, taking no layout room. For a grid cell,
/// where an extra line would push the rows down. It is not a window: it cannot take focus, and a hover
/// tooltip on the field still shows.
void DrawInlineParseErrorBelowItem(const TrackerParseExplain::ParseMessage& msg);

/// The same overlay under an explicit screen rectangle, for a composite widget whose last item is not the
/// input (the duration field ends in a "▼" button). `rectMin`/`rectMax` are the input's screen bounds.
void DrawInlineParseErrorBelowRect(const TrackerParseExplain::ParseMessage& msg, const ImVec2& rectMin,
                                   const ImVec2& rectMax);

/// The same error overlay for a message that is already plain text (the tracker's own reason a save was rejected).
void DrawInlineTextErrorBelowRect(const std::string& text, const ImVec2& rectMin, const ImVec2& rectMax);

/// A dim preview painted under `rectMin`/`rectMax` the way the error overlay is ("= 2026-11-20" under "tomorrow").
void DrawInlineParseHintBelowRect(const std::string& text, const ImVec2& rectMin, const ImVec2& rectMax);

/// True when the field should show its error now: it has the caret or was edited, and is not blank. A field
/// the user has not touched, or that is blank, stays quiet; a required-field rule is a separate message.
inline bool ShouldShowParseError(bool fieldActiveOrEdited, bool textIsBlank) {
    return fieldActiveOrEdited && !textIsBlank;
}

#if defined(SMATCHET_BUILD_UI_TESTS)
/// What the helpers drew in the current or the previous frame (the test engine reads between frames), or
/// empty. `Kind` is "error", "hint" or "overlay".
std::string InlineErrorTextForTests();
std::string InlineErrorKindForTests();
/// Forgets what was drawn so a case starts clean.
void ResetInlineErrorForTests();
#endif

} // namespace ui
} // namespace smatchet
