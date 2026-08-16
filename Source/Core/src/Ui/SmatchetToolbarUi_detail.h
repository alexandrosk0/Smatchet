#ifndef SMATCHET_UI_TOOLBAR_UI_DETAIL_H
#define SMATCHET_UI_TOOLBAR_UI_DETAIL_H

// Pure (ImGui-free) helpers extracted from SmatchetToolbarUi so the draw bodies
// stay layout-only and the value-formatting / index math / shortcut resolution is
// bucket-A testable. See docs/plans/shipped/decompose-top-20-monoliths.md.

#include <string>
#include <vector>

#include "Config/KeybindingsConfig.h" // BoundHotkeyDisplayAll — tooltip shortcut resolution
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

// Shortcut text for one toolbar button's hover tooltip: every combo bound to the
// button's OWN (CommandId, ArgsJson) identity — the very pair DispatchButton
// dispatches. The args matter: BoundHotkeyDisplayAll matches args semantically and
// has NO command-id-only fallback, so resolving against a hardcoded "{}" returned
// nothing for a button bound with non-default args (e.g. `view.sidebar.primary
// {"action":"toggle"}`) and the tooltip silently lost its shortcut. Empty for
// separators, Lua buttons, an unset command id, or an unbound command.
inline std::string ToolbarButtonShortcutDisplay(const std::vector<Keybinding>& bindings, const ToolbarButton& b) {
    if (b.Kind != ToolbarButtonKind::Command || b.CommandId.empty()) {
        return std::string();
    }
    return BoundHotkeyDisplayAll(bindings, b.CommandId, b.ArgsJson);
}

} // namespace toolbar_editor
} // namespace smatchet

#endif // SMATCHET_UI_TOOLBAR_UI_DETAIL_H
