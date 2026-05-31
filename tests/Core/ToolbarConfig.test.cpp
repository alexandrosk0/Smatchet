// Pure-logic tests for the customizable toolbar data model:
// JSON round-trip + effective-toolbar resolution (separator dedup / trim).
// docs/plans/shipped/customizable-icon-toolbar.md § Verification (Bucket A).

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "ToolbarConfig.h"

namespace {

ToolbarButton Cmd(const std::string& id) {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Command;
    b.CommandId = id;
    return b;
}

ToolbarButton Sep() {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Separator;
    return b;
}

} // namespace

TEST_CASE("ToolbarButton JSON round-trip preserves every field") {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Lua;
    b.IconName = "rocket";
    b.IconGlyph = "\xef\x84\x35";
    b.Tooltip = "Run script";
    b.CommandId = "ignored.when.lua";
    b.ArgsJson = "{\"x\":1}";
    b.LuaCode = "print('hi')";

    const nlohmann::json j = b;
    const ToolbarButton r = j.get<ToolbarButton>();

    CHECK(r.Kind == ToolbarButtonKind::Lua);
    CHECK(r.IconName == "rocket");
    CHECK(r.IconGlyph == b.IconGlyph);
    CHECK(r.Tooltip == "Run script");
    CHECK(r.CommandId == "ignored.when.lua");
    CHECK(r.ArgsJson == "{\"x\":1}");
    CHECK(r.LuaCode == "print('hi')");
}

TEST_CASE("ToolbarConfig JSON round-trip preserves visibility + buttons") {
    ToolbarConfig c;
    c.Visible = false;
    c.Buttons.push_back(Cmd("view.list"));
    c.Buttons.push_back(Sep());
    c.Buttons.push_back(Cmd("app.set_readonly"));

    const nlohmann::json j = c;
    const ToolbarConfig r = j.get<ToolbarConfig>();

    CHECK(r.Visible == false);
    REQUIRE(r.Buttons.size() == 3);
    CHECK(r.Buttons[0].CommandId == "view.list");
    CHECK(r.Buttons[1].Kind == ToolbarButtonKind::Separator);
    CHECK(r.Buttons[2].CommandId == "app.set_readonly");
}

TEST_CASE("ToolbarButton.from_json tolerates missing fields and bad kind") {
    nlohmann::json j = nlohmann::json::object();
    j["command_id"] = "x.y";
    j["kind"] = 99; // out of range → coerced to Command
    const ToolbarButton b = j.get<ToolbarButton>();
    CHECK(b.Kind == ToolbarButtonKind::Command);
    CHECK(b.CommandId == "x.y");
    CHECK(b.ArgsJson == "{}"); // default applied
}

TEST_CASE("ToolbarConfig::Default is non-empty and visible") {
    const ToolbarConfig d = ToolbarConfig::Default();
    CHECK(d.Visible == true);
    CHECK(d.Buttons.size() >= 4);
}

TEST_CASE("ResolveEffectiveToolbar: empty append yields the global list (trimmed)") {
    ToolbarConfig g;
    g.Buttons.push_back(Cmd("a"));
    g.Buttons.push_back(Cmd("b"));
    const std::vector<ToolbarButton> out = ResolveEffectiveToolbar(g, {});
    REQUIRE(out.size() == 2);
    CHECK(out[0].CommandId == "a");
    CHECK(out[1].CommandId == "b");
}

TEST_CASE("ResolveEffectiveToolbar: append inserts exactly one separator between segments") {
    ToolbarConfig g;
    g.Buttons.push_back(Cmd("a"));
    std::vector<ToolbarButton> append;
    append.push_back(Cmd("z"));

    const std::vector<ToolbarButton> out = ResolveEffectiveToolbar(g, append);
    REQUIRE(out.size() == 3);
    CHECK(out[0].CommandId == "a");
    CHECK(out[1].Kind == ToolbarButtonKind::Separator);
    CHECK(out[2].CommandId == "z");
}

TEST_CASE("ResolveEffectiveToolbar: collapses doubled boundary separators") {
    ToolbarConfig g;
    g.Buttons.push_back(Cmd("a"));
    g.Buttons.push_back(Sep()); // global already ends in a separator
    std::vector<ToolbarButton> append;
    append.push_back(Sep()); // append starts with a separator
    append.push_back(Cmd("z"));

    const std::vector<ToolbarButton> out = ResolveEffectiveToolbar(g, append);
    REQUIRE(out.size() == 3);
    CHECK(out[0].CommandId == "a");
    CHECK(out[1].Kind == ToolbarButtonKind::Separator);
    CHECK(out[2].CommandId == "z");
}

TEST_CASE("ResolveEffectiveToolbar: trims leading and trailing separators") {
    ToolbarConfig g;
    g.Buttons.push_back(Sep());
    g.Buttons.push_back(Cmd("a"));
    g.Buttons.push_back(Sep());
    g.Buttons.push_back(Sep());
    const std::vector<ToolbarButton> out = ResolveEffectiveToolbar(g, {});
    REQUIRE(out.size() == 1);
    CHECK(out[0].CommandId == "a");
}

namespace {

// CommandIds of the non-separator buttons in `buttons`, in order.
std::vector<std::string> RealIds(const std::vector<ToolbarButton>& buttons) {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (buttons[i].Kind != ToolbarButtonKind::Separator) {
            ids.push_back(buttons[i].CommandId);
        }
    }
    return ids;
}

} // namespace

TEST_CASE("ResolveEffectiveToolbar preserves the global non-separator subsequence") {
    // RenderBar's per-button right-click menu maps the k-th on-screen non-separator button back
    // to the k-th non-separator in cfg.Toolbar.Buttons. That mapping is correct only because
    // resolution drops or merges separators without reordering real buttons. Lock that property.
    ToolbarConfig g;
    g.Buttons.push_back(Sep());   // leading, trimmed
    g.Buttons.push_back(Cmd("a"));
    g.Buttons.push_back(Sep());   // interior, kept
    g.Buttons.push_back(Cmd("b"));
    g.Buttons.push_back(Cmd("c"));
    g.Buttons.push_back(Sep());   // trailing, trimmed

    const std::vector<ToolbarButton> eff = ResolveEffectiveToolbar(g, {});
    CHECK(RealIds(eff) == RealIds(g.Buttons)); // same real buttons, same order
}

TEST_CASE("ResolveEffectiveToolbar keeps global buttons first when an append list is present") {
    // With a per-tracker append, only the leading (global) on-screen buttons map to the global
    // list; appended buttons must come strictly after, so they fall outside the mapping and get
    // no global context menu (the render code guards them with src == -1).
    ToolbarConfig g;
    g.Buttons.push_back(Cmd("g0"));
    g.Buttons.push_back(Cmd("g1"));
    std::vector<ToolbarButton> append;
    append.push_back(Cmd("t0"));
    append.push_back(Cmd("t1"));

    const std::vector<std::string> real = RealIds(ResolveEffectiveToolbar(g, append));
    REQUIRE(real.size() == 4);
    CHECK(real[0] == "g0");
    CHECK(real[1] == "g1");
    CHECK(real[2] == "t0");
    CHECK(real[3] == "t1");
}
