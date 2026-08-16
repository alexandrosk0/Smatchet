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

// --- Toolbar tooltip shortcut resolution (issue #2062) -----------------------------
// The tooltip must resolve the shortcut against the button's OWN (CommandId, ArgsJson)
// — the exact pair DispatchButton dispatches. BoundHotkeyDisplayAll matches args
// semantically and has NO command-id-only fallback, so a hardcoded "{}" silently
// dropped the shortcut for every customised button bound with non-default args.

using smatchet::toolbar_editor::ToolbarButtonShortcutDisplay;

namespace {

Keybinding MakeBinding(const std::string& commandId, const std::string& argsJson, const std::string& hotkey) {
    Keybinding b;
    b.CommandId = commandId;
    b.ArgsJson = argsJson;
    b.Hotkeys.push_back(hotkey);
    b.Enabled = true;
    return b;
}

} // namespace

TEST_CASE("ToolbarButtonShortcutDisplay: a button bound with non-default args keeps its shortcut") {
    std::vector<Keybinding> bindings;
    bindings.push_back(MakeBinding("view.sidebar.primary", "{\"action\":\"toggle\"}", "Ctrl+1"));

    ToolbarButton b = MakeCmd("view.sidebar.primary");
    b.ArgsJson = "{\"action\":\"toggle\"}";
    CHECK(ToolbarButtonShortcutDisplay(bindings, b) == "Ctrl+1");

    // The regression: resolving against the default-args identity finds nothing at all,
    // because the args match is semantic and unbacked by a command-id-only fallback.
    ToolbarButton defaultArgs = MakeCmd("view.sidebar.primary");
    CHECK(ToolbarButtonShortcutDisplay(bindings, defaultArgs).empty());
}

TEST_CASE("ToolbarButtonShortcutDisplay: args are matched semantically, not by string") {
    std::vector<Keybinding> bindings;
    bindings.push_back(MakeBinding("view.toggle.performance", "{\"action\":\"show\",\"pane\":2}", "F9"));

    ToolbarButton b = MakeCmd("view.toggle.performance");
    b.ArgsJson = "{\"pane\":2,\"action\":\"show\"}"; // same object, different key order
    CHECK(ToolbarButtonShortcutDisplay(bindings, b) == "F9");
}

TEST_CASE("ToolbarButtonShortcutDisplay: default-args buttons still resolve, empty args == {}") {
    std::vector<Keybinding> bindings;
    bindings.push_back(MakeBinding("view.toggle.log", "{}", "Ctrl+L"));

    CHECK(ToolbarButtonShortcutDisplay(bindings, MakeCmd("view.toggle.log")) == "Ctrl+L");

    ToolbarButton explicitEmpty = MakeCmd("view.toggle.log");
    explicitEmpty.ArgsJson = "";
    CHECK(ToolbarButtonShortcutDisplay(bindings, explicitEmpty) == "Ctrl+L");
}

TEST_CASE("ToolbarButtonShortcutDisplay: every alias of the matched binding is joined") {
    std::vector<Keybinding> bindings;
    Keybinding b = MakeBinding("ui.zoom.in", "{}", "Ctrl+=");
    b.Hotkeys.push_back("Ctrl+NumAdd");
    bindings.push_back(b);
    CHECK(ToolbarButtonShortcutDisplay(bindings, MakeCmd("ui.zoom.in")) == "Ctrl+= / Ctrl+NumAdd");
}

TEST_CASE("ToolbarButtonShortcutDisplay: empty for non-command buttons and unbound commands") {
    std::vector<Keybinding> bindings;
    bindings.push_back(MakeBinding("view.toggle.log", "{}", "Ctrl+L"));

    ToolbarButton sep;
    sep.Kind = ToolbarButtonKind::Separator;
    CHECK(ToolbarButtonShortcutDisplay(bindings, sep).empty());

    ToolbarButton lua;
    lua.Kind = ToolbarButtonKind::Lua;
    lua.LuaCode = "print(1)";
    CHECK(ToolbarButtonShortcutDisplay(bindings, lua).empty());

    CHECK(ToolbarButtonShortcutDisplay(bindings, MakeCmd("")).empty());
    CHECK(ToolbarButtonShortcutDisplay(bindings, MakeCmd("no.such.command")).empty());
}
