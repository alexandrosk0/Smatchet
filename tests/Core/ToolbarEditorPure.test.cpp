// Pure-logic tests for the helpers extracted from SmatchetToolbarUi::RenderEditor during the
// function-size decomposition: editor row-label formatting + drag-drop destination index math.
// docs/guides/imgui-draw-pattern.md (section-helper pattern, bucket-A pure helpers).

#include <doctest/doctest.h>

#include "SmatchetToolbarUi_detail.h"

#include <string>
#include <vector>

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

using smatchet::toolbar_editor::ArgsJsonStatus;
using smatchet::toolbar_editor::ArgsTextRuns;
using smatchet::toolbar_editor::EditorRowLabelWithArgsCue;
using smatchet::toolbar_editor::ParseToolbarArgsJson;
using smatchet::toolbar_editor::RankCommandMatches;
using smatchet::toolbar_editor::TrimAscii;

TEST_CASE("TrimAscii: strips both ends and keeps inner whitespace") {
    CHECK(TrimAscii("") == "");
    CHECK(TrimAscii(" \t\r\n") == "");
    CHECK(TrimAscii("  a b\t") == "a b");
    CHECK(TrimAscii("x") == "x");
}

TEST_CASE("ParseToolbarArgsJson: empty and whitespace-only text is Ok and means {}") {
    nlohmann::json out = nlohmann::json::array();
    CHECK(ParseToolbarArgsJson("", out) == ArgsJsonStatus::Ok);
    CHECK(out == nlohmann::json::object());
    out = nlohmann::json::array();
    CHECK(ParseToolbarArgsJson("  \n\t", out) == ArgsJsonStatus::Ok);
    CHECK(out == nlohmann::json::object());
}

TEST_CASE("ParseToolbarArgsJson: a JSON object is Ok and returned, surrounding whitespace ignored") {
    nlohmann::json out;
    CHECK(ParseToolbarArgsJson("  {\"action\":\"toggle\"}\n", out) == ArgsJsonStatus::Ok);
    REQUIRE(out.is_object());
    CHECK(out.value("action", std::string()) == "toggle");
    CHECK(ParseToolbarArgsJson("{}", out) == ArgsJsonStatus::Ok);
    CHECK(out == nlohmann::json::object());
}

TEST_CASE("ParseToolbarArgsJson: single quotes, a trailing comma and bare words are Invalid") {
    const std::vector<std::string> bad = {"{'a':1}", "{\"a\":1,}", "action=toggle", "{"};
    for (const std::string& text : bad) {
        CAPTURE(text);
        nlohmann::json out = nlohmann::json::array();
        CHECK(ParseToolbarArgsJson(text, out) == ArgsJsonStatus::Invalid);
        CHECK(out == nlohmann::json::object());
    }
}

TEST_CASE("ParseToolbarArgsJson: valid JSON that is not an object is NotObject") {
    const std::vector<std::string> notObjects = {"[1]", "5", "\"x\"", "null", "true"};
    for (const std::string& text : notObjects) {
        CAPTURE(text);
        nlohmann::json out = nlohmann::json::array();
        CHECK(ParseToolbarArgsJson(text, out) == ArgsJsonStatus::NotObject);
        CHECK(out == nlohmann::json::object());
    }
}

TEST_CASE("ParseToolbarArgsJson: text past the parser's byte cap is TooLarge") {
    nlohmann::json out;
    const std::string big = "{\"a\":\"" + std::string(smatchet::json_safe::kDefaultMaxBytes, 'x') + "\"}";
    CHECK(ParseToolbarArgsJson(big, out) == ArgsJsonStatus::TooLarge);
    CHECK(out == nlohmann::json::object());
}

TEST_CASE("ParseToolbarArgsJson: nesting past the parser's depth cap is TooDeep") {
    nlohmann::json out;
    CHECK(ParseToolbarArgsJson(std::string(300, '[') + std::string(300, ']'), out) == ArgsJsonStatus::TooDeep);
    CHECK(out == nlohmann::json::object());
}

TEST_CASE("RankCommandMatches: an empty or whitespace-only query keeps every entry in order") {
    const std::vector<std::string> names = {"c.z", "a.x", "b.y"};
    const std::vector<int> all = {0, 1, 2};
    CHECK(RankCommandMatches(names, names, "") == all);
    CHECK(RankCommandMatches(names, names, "   ") == all);
}

TEST_CASE("RankCommandMatches: the match ignores case") {
    const std::vector<std::string> names = {"tracker.refresh", "view.list"};
    const std::vector<int> first = {0};
    CHECK(RankCommandMatches(names, names, "REFRESH") == first);
    CHECK(RankCommandMatches(names, names, "Tracker.Refresh") == first);
}

TEST_CASE("EditorRowLabelWithArgsCue: only a bad-args row carries the cue") {
    CHECK(EditorRowLabelWithArgsCue("[ ]  Refresh", false, "(args not valid)") == "[ ]  Refresh");
    CHECK(EditorRowLabelWithArgsCue("[ ]  Refresh", true, "(args not valid)") == "[ ]  Refresh  (args not valid)");
    CHECK(EditorRowLabelWithArgsCue("[ ]  Refresh", true, "") == "[ ]  Refresh");
}

TEST_CASE("ArgsTextRuns: empty text and JSON objects run, anything else does not") {
    CHECK(ArgsTextRuns(""));
    CHECK(ArgsTextRuns("{}"));
    CHECK(ArgsTextRuns("   "));
    CHECK(ArgsTextRuns("{\"action\": \"toggle\"}"));
    CHECK_FALSE(ArgsTextRuns("[1]"));
    CHECK_FALSE(ArgsTextRuns("{'a':1}"));
    CHECK_FALSE(ArgsTextRuns("7"));
}

TEST_CASE("RankCommandMatches: characters in order match, not only a substring") {
    const std::vector<std::string> names = {"tracker.refresh", "view.list"};
    const std::vector<int> first = {0};
    const std::vector<int> second = {1};
    CHECK(RankCommandMatches(names, names, "trref") == first);
    CHECK(RankCommandMatches(names, names, "vwlst") == second);
    CHECK(RankCommandMatches(names, names, "zzz").empty());
}

TEST_CASE("RankCommandMatches: the query is trimmed") {
    const std::vector<std::string> names = {"tracker.refresh", "view.list"};
    CHECK(RankCommandMatches(names, names, "  refresh  ") == RankCommandMatches(names, names, "refresh"));
    CHECK(RankCommandMatches(names, names, "  refresh  ").size() == 1);
}

TEST_CASE("RankCommandMatches: the title in the haystack matches too") {
    const std::vector<std::string> names = {"view.dash", "view.list"};
    const std::vector<std::string> haystacks = {"view.dash Views dashboard", "view.list List views"};
    const std::vector<int> first = {0};
    CHECK(RankCommandMatches(names, haystacks, "dashboard") == first);
    // A haystack list shorter than the names falls back to the name.
    const std::vector<std::string> shortHaystacks = {"view.dash Views dashboard"};
    const std::vector<int> second = {1};
    CHECK(RankCommandMatches(names, shortHaystacks, "list") == second);
}

TEST_CASE("RankCommandMatches: best score first, equal scores by name") {
    const std::vector<std::string> names = {"view.list.refresh.all.panes", "refresh"};
    const std::vector<int> ranked = RankCommandMatches(names, names, "refresh");
    REQUIRE(ranked.size() == 2);
    const int s0 = smatchet::cmd::FuzzyScore("refresh", names[static_cast<std::size_t>(ranked[0])]);
    const int s1 = smatchet::cmd::FuzzyScore("refresh", names[static_cast<std::size_t>(ranked[1])]);
    CHECK(s0 >= s1);
    CHECK(ranked[0] == 1);

    // Same shape, same length: the scores tie, so the name decides.
    const std::vector<std::string> tied = {"b.run", "a.run"};
    REQUIRE(smatchet::cmd::FuzzyScore("run", tied[0]) == smatchet::cmd::FuzzyScore("run", tied[1]));
    const std::vector<int> byName = {1, 0};
    CHECK(RankCommandMatches(tied, tied, "run") == byName);
}
