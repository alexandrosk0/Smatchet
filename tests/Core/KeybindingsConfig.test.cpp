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
    const char* hotkey;
    const char* commandId;
    const char* argsJson;
};

// Exact parity with KeybindingsConfig::Defaults() — order matters (first-seen wins
// at dispatch). If a default shortcut is added/removed/retargeted, update this table
// in the same change so the parity check stays a real guard, not a rubber stamp.
const ExpectedBinding kExpectedDefaults[] = {
    {"Ctrl+B", "view.sidebar.primary", "{\"action\":\"toggle\"}"},
    {"Ctrl+Alt+B", "view.sidebar.secondary", "{\"action\":\"toggle\"}"},
    {"Ctrl+J", "view.panel.bottom", "{\"action\":\"toggle\"}"},
    {"Ctrl+Shift+A", "view.assistant", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+F", "view.toggle.performance", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+D", "view.toggle.plan_doc_viewer", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+I", "view.toggle.bulk_import", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+X", "view.toggle.bulk_export", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+K", "view.toggle.mcp_server", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+L", "view.toggle.scripts", "{\"action\":\"show\"}"},
    {"Ctrl+,", "view.toggle.preferences", "{\"action\":\"show\"}"},
    {"Ctrl+Alt+D", "app.dock_debug.toggle", "{}"},
    {"F11", "app.fullscreen.toggle", "{}"},
    {"Ctrl+Shift+P", "ui.command_palette", "{}"},
    {"Ctrl+Shift+B", "app.bug_report.open", "{}"},
    // Menu-bar shortcuts wired by keybindings-menu-shortcuts-fix.md.
    {"Ctrl+=", "ui.zoom.in", "{}"},
    {"Ctrl+-", "ui.zoom.out", "{}"},
    {"Ctrl+0", "ui.zoom.reset", "{}"},
    {"Ctrl+Shift+V", "ui.open_view", "{}"},
    {"Ctrl+Shift+G", "grid.clear_selection", "{}"},
    {"Ctrl+O", "view.toggle.views_dashboard", "{\"action\":\"show\",\"via\":\"open_project_view\"}"},
    {"Ctrl+Shift+E", "view.toggle.views_dashboard", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+U", "view.toggle.log", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+M", "view.toggle.backend_audit", "{\"action\":\"show\"}"},
    {"Ctrl+Shift+N", "view.toggle.source_annotate", "{\"action\":\"toggle\"}"},
    // Notifications reveal — added with the notifications → view.toggle.notifications
    // rename (UX critique M8); seeded into existing configs via migrated_menu_shortcuts_v2.
    {"Ctrl+Shift+Y", "view.toggle.notifications", "{\"action\":\"show\"}"},
};

} // namespace

TEST_CASE("KeybindingsConfig::Defaults() reproduces the migrated hardcoded set") {
    const KeybindingsConfig c = KeybindingsConfig::Defaults();
    const size_t expectedCount = sizeof(kExpectedDefaults) / sizeof(kExpectedDefaults[0]);
    REQUIRE(c.Bindings.size() == expectedCount);
    for (size_t i = 0; i < expectedCount; ++i) {
        const Keybinding& b = c.Bindings[i];
        const ExpectedBinding& e = kExpectedDefaults[i];
        CHECK_MESSAGE(b.Hotkey == std::string(e.hotkey), e.hotkey);
        CHECK_MESSAGE(b.CommandId == std::string(e.commandId), e.commandId);
        CHECK_MESSAGE(b.ArgsJson == std::string(e.argsJson), e.hotkey);
        CHECK(b.Enabled);
    }
}

TEST_CASE("Keybinding to_json emits the four wire fields") {
    Keybinding b;
    b.CommandId = "view.toggle.performance";
    b.Hotkey = "Ctrl+Shift+F";
    b.ArgsJson = "{\"action\":\"show\"}";
    b.Enabled = false;

    const nlohmann::json j = b;
    CHECK(j.at("command_id").get<std::string>() == "view.toggle.performance");
    CHECK(j.at("hotkey").get<std::string>() == "Ctrl+Shift+F");
    CHECK(j.at("args_json").get<std::string>() == "{\"action\":\"show\"}");
    CHECK(j.at("enabled").get<bool>() == false);
}

TEST_CASE("Keybinding from_json fills defaults for missing fields") {
    const nlohmann::json empty = nlohmann::json::object();
    const Keybinding b = empty.get<Keybinding>();
    CHECK(b.CommandId.empty());
    CHECK(b.Hotkey.empty());
    CHECK(b.ArgsJson == "{}"); // args_json default is an empty JSON object literal
    CHECK(b.Enabled);          // enabled default is true
}

TEST_CASE("KeybindingsConfig survives a full JSON round-trip") {
    const KeybindingsConfig original = KeybindingsConfig::Defaults();
    const nlohmann::json j = original;
    const KeybindingsConfig back = j.get<KeybindingsConfig>();

    REQUIRE(back.Bindings.size() == original.Bindings.size());
    for (size_t i = 0; i < original.Bindings.size(); ++i) {
        CHECK(back.Bindings[i].CommandId == original.Bindings[i].CommandId);
        CHECK(back.Bindings[i].Hotkey == original.Bindings[i].Hotkey);
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
    CHECK(c.Bindings[0].Hotkey == "Ctrl+Shift+F");
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
    CHECK(c.Bindings[static_cast<size_t>(idx)].Hotkey == "Ctrl+Alt+G");
    CHECK(c.Bindings[static_cast<size_t>(idx)].CommandId == "app.bug_report.open");

    // Re-enable on upsert: a disabled row comes back enabled.
    c.Bindings[static_cast<size_t>(idx)].Enabled = false;
    c.SetBindingHotkey("app.bug_report.open", "{}", "Ctrl+Alt+H");
    CHECK(c.Bindings[static_cast<size_t>(idx)].Enabled);

    // New action — appends.
    const int added = c.SetBindingHotkey("view.export", "{}", "Ctrl+E");
    CHECK(added == static_cast<int>(c.Bindings.size()) - 1);
    CHECK(c.Bindings.size() == before + 1);
    CHECK(c.Bindings[static_cast<size_t>(added)].Hotkey == "Ctrl+E");
    CHECK(c.Bindings[static_cast<size_t>(added)].Enabled);
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
    b.Hotkey = "Ctrl+O";
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
    b.Hotkey = "Ctrl+=";
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
    b.Hotkey = "Ctrl+=";
    b.ArgsJson = "{}";
    b.Enabled = false;
    bindings.push_back(b);
    CHECK(BoundHotkeyDisplayForArgs(bindings, "ui.zoom.in", "{}").empty());
}
