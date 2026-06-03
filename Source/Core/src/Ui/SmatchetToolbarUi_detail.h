#ifndef SMATCHET_UI_TOOLBAR_UI_DETAIL_H
#define SMATCHET_UI_TOOLBAR_UI_DETAIL_H

// Pure (ImGui-free) helpers extracted from SmatchetToolbarUi::RenderEditor so the
// editor draw body stays layout-only and the value-formatting / index math is
// bucket-A testable. See docs/plans/active/decompose-top-20-monoliths.md.

#include <string>

#include "Config/ToolbarConfig.h"
#include "Ui/SmatchetIconPickerUi.h" // SmatchetToolbarIconGlyph

namespace smatchet {
namespace toolbar_editor {

// Label shown for one button row in the editor's left-hand list. Separators get a
// fixed marker; real buttons get an icon prefix (a FontAwesome glyph when loaded,
// otherwise a bracket placeholder) plus the tooltip, the command id, or a fallback.
inline std::string EditorRowLabel(const ToolbarButton& b, bool faLoaded) {
    if (b.Kind == ToolbarButtonKind::Separator) {
        return "--- separator ---";
    }
    const std::string glyph = b.IconGlyph.empty() ? SmatchetToolbarIconGlyph(b.IconName) : b.IconGlyph;
    std::string label = (faLoaded && !glyph.empty()) ? (glyph + "  ") : std::string("[ ]  ");
    if (!b.Tooltip.empty()) {
        label += b.Tooltip;
    } else if (!b.CommandId.empty()) {
        label += b.CommandId;
    } else {
        label += "(unset)";
    }
    return label;
}

// Destination index for a drag-drop reorder that erases `src` then inserts before
// the original target `dst`. Erasing an earlier element shifts the target left by
// one, so the insert position is dst-1 when src precedes dst, dst otherwise.
inline int DragDropDestIndex(int src, int dst) { return (src < dst) ? (dst - 1) : dst; }

} // namespace toolbar_editor
} // namespace smatchet

#endif // SMATCHET_UI_TOOLBAR_UI_DETAIL_H
