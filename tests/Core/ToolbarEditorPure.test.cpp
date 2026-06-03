// Pure-logic tests for the helpers extracted from SmatchetToolbarUi::RenderEditor during the
// function-size decomposition: editor row-label formatting + drag-drop destination index math.
// docs/guides/imgui-draw-pattern.md (section-helper pattern, bucket-A pure helpers).

#include <doctest/doctest.h>

#include "SmatchetToolbarUi_detail.h"

namespace {

ToolbarButton MakeCmd(const std::string& id, const std::string& tip = std::string()) {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Command;
    b.CommandId = id;
    b.Tooltip = tip;
    return b;
}

} // namespace

using smatchet::toolbar_editor::DragDropDestIndex;
using smatchet::toolbar_editor::EditorRowLabel;

TEST_CASE("EditorRowLabel: separator renders a fixed marker") {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Separator;
    CHECK(EditorRowLabel(b, true) == "--- separator ---");
    CHECK(EditorRowLabel(b, false) == "--- separator ---");
}

TEST_CASE("EditorRowLabel: tooltip wins over command id") {
    const ToolbarButton b = MakeCmd("view.list", "My Button");
    // FontAwesome off → placeholder icon prefix.
    CHECK(EditorRowLabel(b, false) == "[ ]  My Button");
}

TEST_CASE("EditorRowLabel: falls back to command id then (unset)") {
    CHECK(EditorRowLabel(MakeCmd("view.list"), false) == "[ ]  view.list");
    CHECK(EditorRowLabel(MakeCmd(""), false) == "[ ]  (unset)");
}

TEST_CASE("EditorRowLabel: explicit glyph used as prefix when FontAwesome is loaded") {
    ToolbarButton b = MakeCmd("view.list", "Tip");
    b.IconGlyph = "X"; // stand-in glyph; real glyphs are multi-byte UTF-8
    CHECK(EditorRowLabel(b, true) == "X  Tip");
    // FontAwesome off → glyph suppressed, placeholder used instead.
    CHECK(EditorRowLabel(b, false) == "[ ]  Tip");
}

TEST_CASE("DragDropDestIndex: dragging down shifts target left by one after erase") {
    // src precedes dst: erasing src first slides the target down one slot.
    CHECK(DragDropDestIndex(0, 3) == 2);
    CHECK(DragDropDestIndex(2, 5) == 4);
}

TEST_CASE("DragDropDestIndex: dragging up keeps the target index") {
    // src after dst: erase happens past the insertion point, so dst is unchanged.
    CHECK(DragDropDestIndex(5, 2) == 2);
    CHECK(DragDropDestIndex(3, 0) == 0);
}
