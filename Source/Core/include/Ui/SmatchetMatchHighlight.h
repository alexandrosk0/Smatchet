#pragma once

// The one way a picker row shows which letters matched the typed filter: the theme's selected-text colour
// (ImGuiCol_TextSelectedBg, set by every SmatchetTheme palette) under each matched byte range, the text drawn
// on top. The ranges come from the shared matcher (TextMatchPure.h, docs/plans/input-ux-w2-one-matcher.md).
// Include after imgui.h.

#include "TextMatchPure.h"

#include <cstddef>

struct ImDrawList;
struct ImVec2;
typedef unsigned int ImU32;

namespace SmatchetMatchHighlight {

/// Draws [text, textEnd) at `pos` (whole pixels look best) in `col`, with the match mark under each of the
/// `rangeCount` ranges. Ranges are byte offsets into the text, sorted and not overlapping (what
/// textmatch::Match returns); a range past textEnd is clipped, so stale ranges never read past the text.
/// The caller sets any clip rect. Single-line text: a newline in a range is not marked past its line.
void DrawText(ImDrawList* dl, const ImVec2& pos, ImU32 col, const char* text, const char* textEnd,
              const smatchet::textmatch::ByteRange* ranges, std::size_t rangeCount);

} // namespace SmatchetMatchHighlight
