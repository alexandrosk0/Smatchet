// Pure-logic tests for the rebindable keybinding table: KeybindingsConfig::Defaults()
// parity with the migrated hardcoded shortcut set, JSON round-trip, and the
// malformed-binding skip / missing-field default behaviour of from_json.
// docs/plans/shipped/keyboard-shortcuts-rebindable.md.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "Config/KeybindingsConfig.h"

#include <string>
#include <vector>

namespace {

struct ExpectedBinding {
    // Nullptr-terminated alias set. C++14 aggregate init zero-fills the tail, so a
    // single-combo row still writes just {"Ctrl+B"} and the rest read back as nullptr.
    const char* hotkeys[4];
    const char* commandId;
    const char* argsJson;
};

// Exact parity with KeybindingsConfig::Defaults() — order matters (first-seen wins
// at dispatch). If a default shortcut is added/removed/retargeted, update this table
// in the same change so the parity check stays a real guard, not a rubber stamp.
const ExpectedBinding kExpectedDefaults[] = {
    {{"Ctrl+B"}, "view.sidebar.primary", "{\"action\":\"toggle\"}"},
    {{"Ctrl+Alt+B"}, "view.sidebar.secondary", "{\"action\":\"toggle\"}"},
    {{"Ctrl+J"}, "view.panel.bottom", "{\"action\":\"toggle\"}"},
    {{"Ctrl+Shift+A"}, "view.assistant", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+F"}, "view.toggle.performance", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+D"}, "view.toggle.plan_doc_viewer", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+I"}, "view.toggle.bulk_import", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+X"}, "view.toggle.bulk_export", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+K"}, "view.toggle.mcp_server", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+L"}, "view.toggle.scripts", "{\"action\":\"show\"}"},
    {{"Ctrl+,"}, "view.toggle.preferences", "{\"action\":\"show\"}"},
    {{"Ctrl+Alt+D"}, "app.dock_debug.toggle", "{}"},
    {{"F11"}, "app.fullscreen.toggle", "{}"},
    {{"Ctrl+Shift+P"}, "ui.command_palette", "{}"},
    {{"Ctrl+Shift+B"}, "app.bug_report.open", "{}"},
    // Menu-bar shortcuts wired by keybindings-menu-shortcuts-fix.md.
    // Zoom carries alias combos (docs/plans/keybindings-multi-combo.md): "Ctrl and +"
    // is Ctrl+Shift+= on a US layout, and the keypad variants are layout-independent.
    {{"Ctrl+=", "Ctrl+Shift+=", "Ctrl+NumAdd"}, "ui.zoom.in", "{}"},
    {{"Ctrl+-", "Ctrl+NumSubtract"}, "ui.zoom.out", "{}"},
    {{"Ctrl+0", "Ctrl+Num0"}, "ui.zoom.reset", "{}"},
    {{"Ctrl+Shift+V"}, "ui.open_view", "{}"},
    {{"Ctrl+Shift+G"}, "grid.clear_selection", "{}"},
    {{"Ctrl+O"}, "view.toggle.views_dashboard", "{\"action\":\"show\",\"via\":\"open_project_view\"}"},
    {{"Ctrl+Shift+E"}, "view.toggle.views_dashboard", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+U"}, "view.toggle.log", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+M"}, "view.toggle.backend_audit", "{\"action\":\"show\"}"},
    {{"Ctrl+Shift+N"}, "view.toggle.source_annotate", "{\"action\":\"toggle\"}"},
    // Notifications reveal — added with the notifications → view.toggle.notifications
    // rename (UX critique M8); seeded into existing configs via migrated_menu_shortcuts_v2.
    {{"Ctrl+Shift+Y"}, "view.toggle.notifications", "{\"action\":\"show\"}"},
    // Quick-create issue popup (quick-create-issue-unreal-context plan). Ctrl+Shift+J is
    // reserved by the Unreal host for the overlay visibility toggle — never seed it here.
    {{"Ctrl+Shift+T"}, "issue.quick_create.open", "{}"},
};

} // namespace

TEST_CASE("KeybindingsConfig::Defaults() reproduces the migrated hardcoded set") {
    const KeybindingsConfig c = KeybindingsConfig::Defaults();
    const size_t expectedCount = sizeof(kExpectedDefaults) / sizeof(kExpectedDefaults[0]);
    REQUIRE(c.Bindings.size() == expectedCount);
    for (size_t i = 0; i < expectedCount; ++i) {
        const Keybinding& b = c.Bindings[i];
        const ExpectedBinding& e = kExpectedDefaults[i];
        std::size_t expectedCombos = 0;
        while (expectedCombos < 4U && e.hotkeys[expectedCombos] != nullptr) {
            ++expectedCombos;
        }
        REQUIRE_MESSAGE(b.Hotkeys.size() == expectedCombos, e.hotkeys[0]);
        for (std::size_t h = 0; h < expectedCombos; ++h) {
            CHECK_MESSAGE(b.Hotkeys[h] == std::string(e.hotkeys[h]), e.hotkeys[h]);
        }
        CHECK_MESSAGE(b.CommandId == std::string(e.commandId), e.commandId);
        CHECK_MESSAGE(b.ArgsJson == std::string(e.argsJson), e.hotkeys[0]);
        CHECK(b.Enabled);
    }
}

TEST_CASE("Keybinding to_json emits the wire fields, dual-writing hotkey + hotkeys") {
    Keybinding b;
    b.CommandId = "view.toggle.performance";
    b.Hotkeys.push_back("Ctrl+Shift+F");
    b.ArgsJson = "{\"action\":\"show\"}";
    b.Enabled = false;

    const nlohmann::json j = b;
    CHECK(j.at("command_id").get<std::string>() == "view.toggle.performance");
    CHECK(j.at("hotkey").get<std::string>() == "Ctrl+Shift+F");
    CHECK(j.at("args_json").get<std::string>() == "{\"action\":\"show\"}");
    CHECK(j.at("enabled").get<bool>() == false);
    REQUIRE(j.at("hotkeys").is_array());
    REQUIRE(j.at("hotkeys").size() == 1);
    CHECK(j.at("hotkeys")[0].get<std::string>() == "Ctrl+Shift+F");

    // The legacy scalar carries the PRIMARY combo so an older build reading this file
    // still gets a working shortcut instead of an unbound action.
    Keybinding multi;
    multi.CommandId = "ui.zoom.in";
    multi.Hotkeys.push_back("Ctrl+=");
    multi.Hotkeys.push_back("Ctrl+Shift+=");
    const nlohmann::json jm = multi;
    CHECK(jm.at("hotkey").get<std::string>() == "Ctrl+=");
    CHECK(jm.at("hotkeys").size() == 2);
}

TEST_CASE("Keybinding from_json fills defaults for missing fields") {
    const nlohmann::json empty = nlohmann::json::object();
    const Keybinding b = empty.get<Keybinding>();
    CHECK(b.CommandId.empty());
    CHECK(b.Hotkeys.empty());
    CHECK(b.PrimaryHotkey().empty());
    CHECK(b.ArgsJson == "{}"); // args_json default is an empty JSON object literal
    CHECK(b.Enabled);          // enabled default is true
}

TEST_CASE("Keybinding from_json reads the legacy scalar and the alias list together") {
    // A config written before combos were a list: scalar only -> a one-combo set.
    nlohmann::json legacy = nlohmann::json::object();
    legacy["command_id"] = "ui.zoom.in";
    legacy["hotkey"] = "Ctrl+=";
    const Keybinding fromLegacy = legacy.get<Keybinding>();
    REQUIRE(fromLegacy.Hotkeys.size() == 1);
    CHECK(fromLegacy.Hotkeys[0] == "Ctrl+=");

    // List only -> order preserved.
    nlohmann::json listOnly = nlohmann::json::object();
    listOnly["command_id"] = "ui.zoom.in";
    listOnly["hotkeys"] = nlohmann::json::array({"Ctrl+=", "Ctrl+Shift+=", "Ctrl+NumAdd"});
    const Keybinding fromList = listOnly.get<Keybinding>();
    REQUIRE(fromList.Hotkeys.size() == 3);
    CHECK(fromList.Hotkeys[0] == "Ctrl+=");
    CHECK(fromList.Hotkeys[2] == "Ctrl+NumAdd");

    // Both, with the scalar duplicating hotkeys[0] — the shape a current build writes.
    // Must de-dup, so a save/load round-trip is a no-op rather than growing the set.
    nlohmann::json both = nlohmann::json::object();
    both["command_id"] = "ui.zoom.in";
    both["hotkey"] = "Ctrl+=";
    both["hotkeys"] = nlohmann::json::array({"Ctrl+=", "Ctrl+Shift+="});
    const Keybinding fromBoth = both.get<Keybinding>();
    REQUIRE(fromBoth.Hotkeys.size() == 2);
    CHECK(fromBoth.Hotkeys[0] == "Ctrl+=");

    // Both, disjoint (hand-edited): the scalar is appended last, never dropped.
    nlohmann::json disjoint = nlohmann::json::object();
    disjoint["command_id"] = "ui.zoom.in";
    disjoint["hotkey"] = "Ctrl+Up";
    disjoint["hotkeys"] = nlohmann::json::array({"Ctrl+="});
    const Keybinding fromDisjoint = disjoint.get<Keybinding>();
    REQUIRE(fromDisjoint.Hotkeys.size() == 2);
    CHECK(fromDisjoint.Hotkeys[0] == "Ctrl+=");
    CHECK(fromDisjoint.Hotkeys[1] == "Ctrl+Up");

    // Stray non-string / empty entries are skipped, the rest of the set survives.
    nlohmann::json messy = nlohmann::json::object();
    messy["command_id"] = "ui.zoom.in";
    messy["hotkeys"] = nlohmann::json::array({"Ctrl+=", 7, "", "Ctrl+NumAdd"});
    const Keybinding fromMessy = messy.get<Keybinding>();
    REQUIRE(fromMessy.Hotkeys.size() == 2);
    CHECK(fromMessy.Hotkeys[0] == "Ctrl+=");
    CHECK(fromMessy.Hotkeys[1] == "Ctrl+NumAdd");
}

TEST_CASE("KeybindingsConfig survives a full JSON round-trip") {
    const KeybindingsConfig original = KeybindingsConfig::Defaults();
    const nlohmann::json j = original;
    const KeybindingsConfig back = j.get<KeybindingsConfig>();

    REQUIRE(back.Bindings.size() == original.Bindings.size());
    for (size_t i = 0; i < original.Bindings.size(); ++i) {
        CHECK(back.Bindings[i].CommandId == original.Bindings[i].CommandId);
        CHECK(back.Bindings[i].Hotkeys == original.Bindings[i].Hotkeys);
        CHECK(back.Bindings[i].ArgsJson == original.Bindings[i].ArgsJson);
        CHECK(back.Bindings[i].Enabled == original.Bindings[i].Enabled);
    }
}

TEST_CASE("KeybindingsConfig from_json skips malformed bindings, keeps the rest") {
    // One well-formed object, one non-object entry, and one object whose command_id
    // is the wrong JSON type (number, not string) — the latter throws inside
    // j.value() and is caught + skipped. Only the well-formed binding survives.
    const nlohmann::json j = {
        {"bindings", nlohmann::json::array({
                         {{"command_id", "view.toggle.performance"},
                          {"hotkey", "Ctrl+Shift+F"},
                          {"args_json", "{}"},
                          {"enabled", true}},
                         42,                  // non-object — skipped
                         {{"command_id", 5}}, // command_id wrong type — throws, skipped
                     })},
    };
    const KeybindingsConfig c = j.get<KeybindingsConfig>();
    REQUIRE(c.Bindings.size() == 1);
    CHECK(c.Bindings[0].CommandId == "view.toggle.performance");
    CHECK(c.Bindings[0].PrimaryHotkey() == "Ctrl+Shift+F");
}

TEST_CASE("KeybindingsConfig from_json yields an empty table when bindings is absent or not an array") {
    const KeybindingsConfig none = nlohmann::json::object().get<KeybindingsConfig>();
    CHECK(none.Bindings.empty());

    const nlohmann::json notArray = {{"bindings", "nope"}};
    const KeybindingsConfig bad = notArray.get<KeybindingsConfig>();
    CHECK(bad.Bindings.empty());
}

TEST_CASE("FindBindingIndex keys on (commandId, argsJson), not commandId alone") {
    KeybindingsConfig c;
    c.Bindings.push_back(Keybinding());
    c.Bindings[0].CommandId = "view.sidebar.primary";
    c.Bindings[0].ArgsJson = "{\"action\":\"toggle\"}";
    c.Bindings.push_back(Keybinding());
    c.Bindings[1].CommandId = "view.sidebar.primary";
    c.Bindings[1].ArgsJson = "{\"action\":\"show\"}";

    CHECK(c.FindBindingIndex("view.sidebar.primary", "{\"action\":\"toggle\"}") == 0);
    CHECK(c.FindBindingIndex("view.sidebar.primary", "{\"action\":\"show\"}") == 1);
    CHECK(c.FindBindingIndex("view.sidebar.primary", "{}") == -1);
    CHECK(c.FindBindingIndex("nope", "{}") == -1);
}

TEST_CASE("SetBindingHotkey upserts: mutates an existing row, appends a new one") {
    KeybindingsConfig c = KeybindingsConfig::Defaults();
    const size_t before = c.Bindings.size();

    // Existing action (app.bug_report.open / "{}") — mutate in place, no growth.
    const int idx = c.SetBindingHotkey("app.bug_report.open", "{}", "Ctrl+Alt+G");
    CHECK(idx >= 0);
    CHECK(c.Bindings.size() == before);
    CHECK(c.Bindings[static_cast<size_t>(idx)].PrimaryHotkey() == "Ctrl+Alt+G");
    CHECK(c.Bindings[static_cast<size_t>(idx)].CommandId == "app.bug_report.open");

    // Re-enable on upsert: a disabled row comes back enabled.
    c.Bindings[static_cast<size_t>(idx)].Enabled = false;
    c.SetBindingHotkey("app.bug_report.open", "{}", "Ctrl+Alt+H");
    CHECK(c.Bindings[static_cast<size_t>(idx)].Enabled);

    // New action — appends.
    const int added = c.SetBindingHotkey("view.export", "{}", "Ctrl+E");
    CHECK(added == static_cast<int>(c.Bindings.size()) - 1);
    CHECK(c.Bindings.size() == before + 1);
    CHECK(c.Bindings[static_cast<size_t>(added)].PrimaryHotkey() == "Ctrl+E");
    CHECK(c.Bindings[static_cast<size_t>(added)].Enabled);
}

TEST_CASE("SetBindingHotkey REPLACES the alias set, it does not append") {
    // "This action's shortcut is now X" — the quick-bind popup and every config
    // migration mean exactly that, so a multi-combo action collapses to one combo.
    // AddBindingHotkey is the additive door.
    KeybindingsConfig c = KeybindingsConfig::Defaults();
    const int zi = c.FindBindingIndex("ui.zoom.in", "{}");
    REQUIRE(zi >= 0);
    REQUIRE(c.Bindings[static_cast<size_t>(zi)].Hotkeys.size() == 3);

    c.SetBindingHotkey("ui.zoom.in", "{}", "Ctrl+Up");
    REQUIRE(c.Bindings[static_cast<size_t>(zi)].Hotkeys.size() == 1);
    CHECK(c.Bindings[static_cast<size_t>(zi)].Hotkeys[0] == "Ctrl+Up");

    // An empty hotkey leaves the row listed but unbound (still rebindable in the editor).
    c.SetBindingHotkey("ui.zoom.in", "{}", "");
    CHECK(c.Bindings[static_cast<size_t>(zi)].Hotkeys.empty());
    CHECK(c.FindBindingIndex("ui.zoom.in", "{}") == zi);
}

TEST_CASE("AddBindingHotkey / RemoveBindingHotkey manage one alias at a time") {
    KeybindingsConfig c = KeybindingsConfig::Defaults();
    const int zi = c.FindBindingIndex("ui.zoom.out", "{}");
    REQUIRE(zi >= 0);
    const size_t rows = c.Bindings.size();
    REQUIRE(c.Bindings[static_cast<size_t>(zi)].Hotkeys.size() == 2);

    CHECK(c.AddBindingHotkey("ui.zoom.out", "{}", "Ctrl+Down") == zi);
    CHECK(c.Bindings[static_cast<size_t>(zi)].Hotkeys.size() == 3);
    CHECK(c.Bindings.size() == rows); // adds a combo, not a row

    // Duplicate add is a no-op on the set.
    c.AddBindingHotkey("ui.zoom.out", "{}", "Ctrl+Down");
    CHECK(c.Bindings[static_cast<size_t>(zi)].Hotkeys.size() == 3);

    // Empty is rejected outright.
    CHECK(c.AddBindingHotkey("ui.zoom.out", "{}", "") == -1);

    // Adding to an unknown action appends a fresh row.
    const int added = c.AddBindingHotkey("view.export", "{}", "Ctrl+E");
    CHECK(added == static_cast<int>(c.Bindings.size()) - 1);
    CHECK(c.Bindings.size() == rows + 1);

    // Adding re-enables a disabled action.
    c.Bindings[static_cast<size_t>(zi)].Enabled = false;
    c.AddBindingHotkey("ui.zoom.out", "{}", "Ctrl+PageDown");
    CHECK(c.Bindings[static_cast<size_t>(zi)].Enabled);

    CHECK(c.RemoveBindingHotkey("ui.zoom.out", "{}", "Ctrl+Down"));
    CHECK(c.Bindings[static_cast<size_t>(zi)].Hotkeys.size() == 3);
    CHECK_FALSE(c.Bindings[static_cast<size_t>(zi)].HasHotkey("Ctrl+Down"));
    CHECK_FALSE(c.RemoveBindingHotkey("ui.zoom.out", "{}", "Ctrl+Down")); // already gone
    CHECK_FALSE(c.RemoveBindingHotkey("nope", "{}", "Ctrl+Down"));        // unknown action

    // Removing the LAST combo keeps the row listed and unbound — same contract as
    // "Clear", so the action stays visible and rebindable in the editor.
    while (!c.Bindings[static_cast<size_t>(zi)].Hotkeys.empty()) {
        c.RemoveBindingHotkey("ui.zoom.out", "{}", c.Bindings[static_cast<size_t>(zi)].Hotkeys[0]);
    }
    CHECK(c.FindBindingIndex("ui.zoom.out", "{}") == zi);
}

TEST_CASE("Keybinding combo accessors dedup, preserve order, and report outcomes") {
    Keybinding b;
    CHECK(b.PrimaryHotkey().empty());
    CHECK_FALSE(b.HasHotkey("Ctrl+="));

    CHECK(b.AddHotkey("Ctrl+="));
    CHECK(b.AddHotkey("Ctrl+Shift+="));
    CHECK_FALSE(b.AddHotkey("Ctrl+=")); // duplicate
    CHECK_FALSE(b.AddHotkey(""));       // empty
    REQUIRE(b.Hotkeys.size() == 2);
    CHECK(b.PrimaryHotkey() == "Ctrl+="); // primary is the FIRST combo, insertion-ordered

    CHECK(b.RemoveHotkey("Ctrl+="));
    CHECK(b.PrimaryHotkey() == "Ctrl+Shift+="); // the next alias becomes primary
    CHECK_FALSE(b.RemoveHotkey("Ctrl+="));
}

TEST_CASE("RemoveBinding erases the matching action and reports the outcome") {
    KeybindingsConfig c = KeybindingsConfig::Defaults();
    const size_t before = c.Bindings.size();

    CHECK(c.RemoveBinding("app.fullscreen.toggle", "{}"));
    CHECK(c.Bindings.size() == before - 1);
    CHECK(c.FindBindingIndex("app.fullscreen.toggle", "{}") == -1);

    // Second remove of the same action is a no-op false.
    CHECK_FALSE(c.RemoveBinding("app.fullscreen.toggle", "{}"));
    CHECK(c.Bindings.size() == before - 1);
}

TEST_CASE("BoundHotkeyDisplayForArgs resolves distinct-args bindings to distinct keys") {
    // One command id, two args, two keys — the menu's "Open Project View" vs
    // "Views Dashboard" case. String equality alone can't tell them apart from the
    // wrong key; semantic args matching must pick the right Hotkey for each.
    const KeybindingsConfig c = KeybindingsConfig::Defaults();
    CHECK(BoundHotkeyDisplayForArgs(c.Bindings, "view.toggle.views_dashboard",
                                    "{\"action\":\"show\",\"via\":\"open_project_view\"}") == "Ctrl+O");
    CHECK(BoundHotkeyDisplayForArgs(c.Bindings, "view.toggle.views_dashboard", "{\"action\":\"show\"}") ==
          "Ctrl+Shift+E");
}

TEST_CASE("BoundHotkeyDisplayForArgs matches args by JSON semantics, not string bytes") {
    std::vector<Keybinding> bindings;
    Keybinding b;
    b.CommandId = "view.toggle.views_dashboard";
    b.Hotkeys.push_back("Ctrl+O");
    b.ArgsJson = "{\"action\":\"show\",\"via\":\"open_project_view\"}";
    b.Enabled = true;
    bindings.push_back(b);

    // Same object, keys in the opposite order — semantic equality must still match.
    CHECK(BoundHotkeyDisplayForArgs(bindings, "view.toggle.views_dashboard",
                                    "{\"via\":\"open_project_view\",\"action\":\"show\"}") == "Ctrl+O");
}

TEST_CASE("BoundHotkeyDisplayForArgs treats empty and \"{}\" args as equivalent") {
    std::vector<Keybinding> bindings;
    Keybinding b;
    b.CommandId = "ui.zoom.in";
    b.Hotkeys.push_back("Ctrl+=");
    b.ArgsJson = "{}";
    b.Enabled = true;
    bindings.push_back(b);

    CHECK(BoundHotkeyDisplayForArgs(bindings, "ui.zoom.in", "{}") == "Ctrl+=");
    CHECK(BoundHotkeyDisplayForArgs(bindings, "ui.zoom.in", "") == "Ctrl+=");

    // An empty-args query must NOT match a non-empty-args binding.
    bindings[0].ArgsJson = "{\"delta\":1}";
    CHECK(BoundHotkeyDisplayForArgs(bindings, "ui.zoom.in", "{}").empty());
}

TEST_CASE("BoundHotkeyDisplayForArgs returns empty for unbound or disabled bindings") {
    const KeybindingsConfig c = KeybindingsConfig::Defaults();
    // No such command id.
    CHECK(BoundHotkeyDisplayForArgs(c.Bindings, "no.such.command", "{}").empty());

    // Right command, wrong args — no match.
    CHECK(BoundHotkeyDisplayForArgs(c.Bindings, "view.toggle.log", "{\"action\":\"hide\"}").empty());

    // A disabled binding is skipped.
    std::vector<Keybinding> bindings;
    Keybinding b;
    b.CommandId = "ui.zoom.in";
    b.Hotkeys.push_back("Ctrl+=");
    b.ArgsJson = "{}";
    b.Enabled = false;
    bindings.push_back(b);
    CHECK(BoundHotkeyDisplayForArgs(bindings, "ui.zoom.in", "{}").empty());
}

TEST_CASE("BoundHotkeyDisplay* surface the PRIMARY combo, BoundHotkeyDisplayAll the whole set") {
    // Menus, palette rows and menu-bar shortcut columns are too narrow for a list, so
    // the single-combo helpers must keep returning exactly one combo even now that an
    // action can carry three. Pinned explicitly so a future "join them all" change to
    // these helpers breaks a test rather than the menu layout.
    const KeybindingsConfig c = KeybindingsConfig::Defaults();
    CHECK(BoundHotkeyDisplay(c.Bindings, "ui.zoom.in") == "Ctrl+=");
    CHECK(BoundHotkeyDisplayForArgs(c.Bindings, "ui.zoom.in", "{}") == "Ctrl+=");

    // Roomy surfaces (toolbar tooltip, quick-bind "also bound" line) get everything.
    CHECK(BoundHotkeyDisplayAll(c.Bindings, "ui.zoom.in", "{}") == "Ctrl+= / Ctrl+Shift+= / Ctrl+NumAdd");
    CHECK(BoundHotkeyDisplayAll(c.Bindings, "ui.zoom.out", "{}", ", ") == "Ctrl+-, Ctrl+NumSubtract");

    // A single-combo action reads the same through both doors.
    CHECK(BoundHotkeyDisplayAll(c.Bindings, "app.fullscreen.toggle", "{}") == "F11");

    // Unbound / unknown / disabled all yield "".
    CHECK(BoundHotkeyDisplayAll(c.Bindings, "no.such.command", "{}").empty());
    std::vector<Keybinding> disabled;
    Keybinding b;
    b.CommandId = "ui.zoom.in";
    b.Hotkeys.push_back("Ctrl+=");
    b.ArgsJson = "{}";
    b.Enabled = false;
    disabled.push_back(b);
    CHECK(BoundHotkeyDisplayAll(disabled, "ui.zoom.in", "{}").empty());
}
