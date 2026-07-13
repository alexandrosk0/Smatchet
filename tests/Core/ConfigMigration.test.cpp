// ConfigMigration tests — exercise the schema-evolution paths in ConfigManager:
// fixture round-trip, idempotent re-migrate, hostile inputs (truncated /
// garbage / unknown-version).
//
// Smatchet's on-disk `smatchet_config.json` does NOT carry an explicit schema
// version field for the broad config — evolution is by-default-value (a new
// field on `TrackerConfig` reads as its default until the user saves once).
// Two explicit version-like markers exist:
//   1. `layout_schema_version` (int) — bumped per `kCurrentLayoutSchemaVersion`
//      when the embedded ImGui dock layout changes incompatibly.
//   2. `migrated_inherit_issuetype_v1` (bool) — one-shot migration flag that
//      injects "issuetype" into both new-issue inherit lists when absent.
//
// Pillar 3 (never crash) is the load-bearing invariant under [high-risk]: every
// hostile fixture must terminate `ConfigManager::Load()` cleanly with sensible
// defaults plus a `LOG_ERROR` (caller can observe via Logger), never abort.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// Read a fixture file under tests/fixtures/config/ relative to the test exe's
// expected working directory. CTest runs from build/<preset>/, so we walk up
// to the source tree to find tests/fixtures/. As a fallback we also try
// SMATCHET_TEST_FIXTURES env var.
std::string ReadFixture(const std::string& name) {
    // Candidate locations to probe, in order. CTest under our presets runs
    // SmatchetTests.exe from build/ninja-test-msys2/tests/, but we may also be
    // launched directly via the script-runner or from the source root.
    const char* envOverride = std::getenv("SMATCHET_TEST_FIXTURES");
    const std::string candidates[] = {
        envOverride ? (std::string(envOverride) + "/" + name) : std::string(),
        "tests/fixtures/config/" + name,
        "../../tests/fixtures/config/" + name,
        "../../../tests/fixtures/config/" + name,
        "../tests/fixtures/config/" + name,
    };
    for (const std::string& path : candidates) {
        if (path.empty())
            continue;
        std::ifstream f(path, std::ios::binary);
        if (f.good()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return std::string();
}

// Drop `contents` at <guarded dir>/smatchet_config.json and force the next Load
// to re-read from disk.
void WriteConfigRaw(const smatchet_tests::TestEnvGuard& env, const std::string& contents) {
    const std::string path = env.DirWithSep() + "smatchet_config.json";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!contents.empty()) {
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    f.close();
    ConfigManager::InvalidateCache();
}

bool Contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("ConfigMigration fixture v5 loads with stable schema shape") {
    smatchet_tests::TestEnvGuard env;
    const std::string raw = ReadFixture("v5.json");
    REQUIRE_FALSE(raw.empty()); // fixture must be present; else test infra is broken
    WriteConfigRaw(env, raw);

    const TrackerConfig cfg = ConfigManager::Load();

    // Schema-stable identity fields round-trip from the fixture.
    CHECK(cfg.TrackerType == "Jira");
    CHECK(cfg.ReadOnlyMode == false);
    // Avoid odr-use of the in-class static const int (no out-of-line definition); compare the
    // loaded value directly against the literal version baked into the fixture (v5).
    // After the v5 → v6 bump (AI assistant feature, Phase E), `kCurrentLayoutSchemaVersion`
    // moved past the fixture's frozen `5`. ConfigManager::Load() preserves the raw int
    // (no auto-upgrade — the runtime callers in SmatchetUI / Source/Standalone main detect
    // the < kCurrentLayoutSchemaVersion case and reset + bump there), so the fixture value
    // remains `5` after Load. The drift assertion now asserts the OLD-fixture invariant
    // rather than a stale "current == fixture" coincidence.
    CHECK(cfg.LayoutSchemaVersion == 5);
    CHECK(cfg.LayoutSchemaVersion < +ConfigManager::kCurrentLayoutSchemaVersion);
    // MigratedInheritIssueTypeV1 was true in the v5 fixture, so the one-shot inject is a no-op
    // and the persisted inherit list survives unchanged.
    CHECK(cfg.MigratedInheritIssueTypeV1 == true);
    CHECK_FALSE(cfg.NewIssueInheritFieldIds.empty());
    CHECK_FALSE(cfg.NewIssueInheritFieldIdsPlane.empty());
    // Sanity: the fixture's MCP port is reflected.
    CHECK(cfg.McpPort == 8765);
}

TEST_CASE("ConfigMigration fixture v4 (pre-issuetype-injection) bumps MigratedInheritIssueTypeV1 to true") {
    smatchet_tests::TestEnvGuard env;
    const std::string raw = ReadFixture("v4.json");
    REQUIRE_FALSE(raw.empty());
    WriteConfigRaw(env, raw);

    const TrackerConfig cfg = ConfigManager::Load();

    // After migration, the flag is set and "issuetype" was injected at the front of both lists.
    CHECK(cfg.MigratedInheritIssueTypeV1 == true);
    REQUIRE_FALSE(cfg.NewIssueInheritFieldIds.empty());
    REQUIRE_FALSE(cfg.NewIssueInheritFieldIdsPlane.empty());
    CHECK(cfg.NewIssueInheritFieldIds.front() == "issuetype");
    CHECK(cfg.NewIssueInheritFieldIdsPlane.front() == "issuetype");
    CHECK(Contains(cfg.NewIssueInheritFieldIds, "issuetype"));
    CHECK(Contains(cfg.NewIssueInheritFieldIdsPlane, "issuetype"));
}

TEST_CASE("ConfigMigration is idempotent on re-Load") {
    smatchet_tests::TestEnvGuard env;
    const std::string raw = ReadFixture("v5.json");
    REQUIRE_FALSE(raw.empty());
    WriteConfigRaw(env, raw);

    const TrackerConfig first = ConfigManager::Load();
    ConfigManager::InvalidateCache();
    const TrackerConfig second = ConfigManager::Load();
    ConfigManager::InvalidateCache();
    const TrackerConfig third = ConfigManager::Load();

    // Migration markers stable across repeated Load() calls.
    CHECK(first.MigratedInheritIssueTypeV1 == second.MigratedInheritIssueTypeV1);
    CHECK(second.MigratedInheritIssueTypeV1 == third.MigratedInheritIssueTypeV1);

    // Inherit lists are stable across reloads (no drift, no duplicate inject).
    CHECK(first.NewIssueInheritFieldIds == second.NewIssueInheritFieldIds);
    CHECK(second.NewIssueInheritFieldIds == third.NewIssueInheritFieldIds);
    CHECK(first.NewIssueInheritFieldIdsPlane == second.NewIssueInheritFieldIdsPlane);
    CHECK(second.NewIssueInheritFieldIdsPlane == third.NewIssueInheritFieldIdsPlane);

    // Top-level identity also stable.
    CHECK(first.TrackerType == second.TrackerType);
    CHECK(first.LayoutSchemaVersion == second.LayoutSchemaVersion);
}

TEST_CASE("ConfigMigration v4 -> v5 -> v5 reload preserves injected issuetype" * doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;
    const std::string raw = ReadFixture("v4.json");
    REQUIRE_FALSE(raw.empty());
    WriteConfigRaw(env, raw);

    // First Load runs the one-shot inject, second Load (after persisting) sees the flag set
    // and skips the inject — the list shape persists.
    const TrackerConfig first = ConfigManager::Load();
    REQUIRE_FALSE(first.NewIssueInheritFieldIds.empty());
    CHECK(first.NewIssueInheritFieldIds.front() == "issuetype");
    const std::size_t firstSize = first.NewIssueInheritFieldIds.size();

    // Persist + reload — Save() writes migrated_inherit_issuetype_v1=true to disk so the
    // second Load() takes the fast path.
    ConfigManager::Save(first);
    ConfigManager::InvalidateCache();
    const TrackerConfig second = ConfigManager::Load();
    CHECK(second.MigratedInheritIssueTypeV1 == true);
    REQUIRE_FALSE(second.NewIssueInheritFieldIds.empty());
    CHECK(second.NewIssueInheritFieldIds.front() == "issuetype");
    // No duplicate inject — count is unchanged across reloads.
    CHECK(second.NewIssueInheritFieldIds.size() == firstSize);
}

TEST_CASE("ConfigMigration hostile input: garbage JSON falls back to defaults" * doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;
    const std::string raw = ReadFixture("garbage.json");
    REQUIRE_FALSE(raw.empty());
    WriteConfigRaw(env, raw);

    // Pillar 3 (never crash): Load() must terminate cleanly and return a sensible default config
    // even when the on-disk file is not a JSON object.
    const TrackerConfig cfg = ConfigManager::Load();

    // Sensible default — empty domain / token / etc., default tracker type, default port.
    CHECK(cfg.Domain.empty());
    CHECK(cfg.Email.empty());
    CHECK(cfg.ApiToken.empty());
    CHECK_FALSE(cfg.McpPort == 0); // default applies (non-zero), see SmatchetDefaults
}

TEST_CASE("ConfigMigration hostile input: truncated JSON mid-key does not crash" * doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;
    const std::string raw = ReadFixture("truncated.json");
    REQUIRE_FALSE(raw.empty());
    WriteConfigRaw(env, raw);

    // Truncated mid-key — parser throws, Load() catches, returns defaults.
    const TrackerConfig cfg = ConfigManager::Load();

    // Same default invariant — empty user secrets, struct defaults survive.
    CHECK(cfg.Domain.empty());
    CHECK(cfg.ApiToken.empty());
}

TEST_CASE("ConfigMigration hostile input: empty file does not crash" * doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;
    WriteConfigRaw(env, std::string()); // zero bytes
    const TrackerConfig cfg = ConfigManager::Load();
    CHECK(cfg.Domain.empty());
    CHECK(cfg.ApiToken.empty());
}

TEST_CASE("ConfigMigration hostile input: unknown future-version layout falls back gracefully" *
          doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;

    // Simulate a future config that bumped layout_schema_version far past kCurrentLayoutSchemaVersion.
    // ConfigManager treats this as "user has a newer schema we don't recognize" — load the raw int
    // and let the caller decide whether to force a re-write. The Load() path must not crash.
    const std::string future = "{\"layout_schema_version\":999,\"tracker_type\":\"Plane\"}";
    WriteConfigRaw(env, future);

    const TrackerConfig cfg = ConfigManager::Load();
    CHECK(cfg.LayoutSchemaVersion == 999);
    CHECK(cfg.TrackerType == "Plane");
}

TEST_CASE("ConfigMigration hostile input: negative layout_schema_version is clamped to 0") {
    smatchet_tests::TestEnvGuard env;
    const std::string negative = "{\"layout_schema_version\":-7}";
    WriteConfigRaw(env, negative);

    const TrackerConfig cfg = ConfigManager::Load();
    // ConfigManager::Load() clamps negative LayoutSchemaVersion to 0 (defensive).
    CHECK(cfg.LayoutSchemaVersion == 0);
}

TEST_CASE("ConfigMigration migrated_inherit_issuetype_v1 missing on disk: one-shot injection") {
    smatchet_tests::TestEnvGuard env;
    // Pre-migration shape: no `migrated_inherit_issuetype_v1` key, no inherit-list keys either.
    const std::string preMigration = "{\"tracker_type\":\"Jira\",\"read_only_mode\":false}";
    WriteConfigRaw(env, preMigration);

    const TrackerConfig cfg = ConfigManager::Load();

    // The one-shot migration ran and "issuetype" is at the front of both inherit lists.
    CHECK(cfg.MigratedInheritIssueTypeV1 == true);
    REQUIRE_FALSE(cfg.NewIssueInheritFieldIds.empty());
    REQUIRE_FALSE(cfg.NewIssueInheritFieldIdsPlane.empty());
    CHECK(cfg.NewIssueInheritFieldIds.front() == "issuetype");
    CHECK(cfg.NewIssueInheritFieldIdsPlane.front() == "issuetype");
}

// --- DeepSeek provider — additive config keys round-trip ---
//
// Mirrors the Anthropic save/load shape. The new keys are additive — older
// configs without them hydrate to the construct-time defaults via
// `j.value(..., default)`. Tests below assert the Save -> Load round-trip
// preserves the values and (on Win32) that a hand-edited legacy plaintext
// `ai_deepseek_api_key` field migrates to `ai_deepseek_api_key_enc` on
// the eager Save() at the bottom of Load().

TEST_CASE("ConfigMigration DeepSeek fields round-trip via Save -> Load") {
    smatchet_tests::TestEnvGuard env;

    TrackerConfig out;
    out.AiProviderKind = 4; // DeepSeek
    out.AiDeepSeekApiKey = "sk-deepseek-roundtrip";
    out.AiDeepSeekBaseUrl = "https://api.deepseek.example/v1";
    out.AiModelDeepSeek = "deepseek-reasoner";
    ConfigManager::Save(out);
    ConfigManager::InvalidateCache();

    const TrackerConfig back = ConfigManager::Load();
    CHECK(back.AiProviderKind == 4);
    CHECK(back.AiDeepSeekApiKey == "sk-deepseek-roundtrip");
    CHECK(back.AiDeepSeekBaseUrl == "https://api.deepseek.example/v1");
    CHECK(back.AiModelDeepSeek == "deepseek-reasoner");
}

TEST_CASE("ConfigMigration DeepSeek keys default when absent from disk") {
    smatchet_tests::TestEnvGuard env;
    // Pre-DeepSeek config: only an unrelated top-level field. The new keys must
    // hydrate to construct-time defaults — empty key, empty base URL, model
    // "deepseek-chat" (the default in TrackerConfig).
    const std::string preDeepSeek = "{\"tracker_type\":\"Jira\"}";
    WriteConfigRaw(env, preDeepSeek);

    const TrackerConfig cfg = ConfigManager::Load();
    CHECK(cfg.AiDeepSeekApiKey.empty());
    CHECK(cfg.AiDeepSeekBaseUrl.empty());
    CHECK(cfg.AiModelDeepSeek == "deepseek-chat");
}

#if defined(_WIN32)
TEST_CASE("ConfigMigration DeepSeek legacy plaintext ai_deepseek_api_key migrates to _enc") {
    smatchet_tests::TestEnvGuard env;
    // Hand-edited legacy shape — operator dropped the plaintext key into the JSON.
    // Load() should read it via the fallback branch, flag the migration, and the
    // eager Save() at the bottom of Load() must re-encrypt to `_enc` + erase the
    // plaintext field on the next read.
    const std::string legacy = "{\"tracker_type\":\"Jira\",\"ai_deepseek_api_key\":\"sk-legacy-deepseek\"}";
    WriteConfigRaw(env, legacy);

    const TrackerConfig first = ConfigManager::Load();
    CHECK(first.AiDeepSeekApiKey == "sk-legacy-deepseek");
    // Re-load from disk after migration — the on-disk JSON should now carry
    // ai_deepseek_api_key_enc and the plaintext field should be gone.
    ConfigManager::InvalidateCache();
    const TrackerConfig second = ConfigManager::Load();
    CHECK(second.AiDeepSeekApiKey == "sk-legacy-deepseek");
}
#endif

TEST_CASE("ConfigMigration migrated_inherit_issuetype_v1=true on disk: injection skipped") {
    smatchet_tests::TestEnvGuard env;
    // User explicitly removed "issuetype" then saved (migrated_v1 stays true), the next
    // Load() must respect their choice — no re-inject.
    const std::string postRemoval = "{\"tracker_type\":\"Jira\",\"read_only_mode\":false,"
                                    "\"migrated_inherit_issuetype_v1\":true,"
                                    "\"new_issue_inherit_field_ids\":[\"priority\",\"assignee\"],"
                                    "\"new_issue_inherit_field_ids_plane\":[\"priority\",\"assignee\"]}";
    WriteConfigRaw(env, postRemoval);

    const TrackerConfig cfg = ConfigManager::Load();

    CHECK(cfg.MigratedInheritIssueTypeV1 == true);
    REQUIRE(cfg.NewIssueInheritFieldIds.size() == 2);
    CHECK(cfg.NewIssueInheritFieldIds[0] == "priority");
    CHECK(cfg.NewIssueInheritFieldIds[1] == "assignee");
    CHECK_FALSE(Contains(cfg.NewIssueInheritFieldIds, "issuetype")); // user-removed; respected
    CHECK_FALSE(Contains(cfg.NewIssueInheritFieldIdsPlane, "issuetype"));
}

// --- Menu-bar shortcut keybindings — one-shot seed migration (migrated_menu_shortcuts_v1) ---
//
// KeybindingsConfig::from_json REPLACES (does not merge) the binding table, so a config saved by a
// build that predates the menu-bar shortcut defaults keeps only the bindings it knew about. Without
// the migration an upgrading user never receives Ctrl+= → ui.zoom.in et al. — the marquee "Zoom In
// doesn't work" symptom. MigrateMenuShortcutKeybindingsV1 seeds the new defaults exactly once,
// append-only, only when the (command_id, args_json) identity is absent — preserving user rebinds
// and never resurrecting a binding the user later cleared.

namespace {
int CountCommand(const KeybindingsConfig& kb, const std::string& commandId) {
    int n = 0;
    for (const Keybinding& b : kb.Bindings) {
        if (b.CommandId == commandId) {
            ++n;
        }
    }
    return n;
}
} // namespace

TEST_CASE("ConfigMigration menu shortcuts: seeds the new bindings into a pre-existing config") {
    smatchet_tests::TestEnvGuard env;
    // Config saved by a build that predates the menu-shortcut defaults: it carries a keybindings
    // block (so from_json replaces the table with just these two), and no migrated flag.
    const std::string pre = R"json({"tracker_type":"Jira","keybindings":{"bindings":[
        {"command_id":"view.sidebar.primary","hotkey":"Ctrl+B","args_json":"{\"action\":\"toggle\"}","enabled":true},
        {"command_id":"app.dock_debug.toggle","hotkey":"Ctrl+Alt+D","args_json":"{}","enabled":true}
    ]}})json";
    WriteConfigRaw(env, pre);

    const TrackerConfig cfg = ConfigManager::Load();

    // Flag flips true and the new menu-bar shortcuts are now present.
    CHECK(cfg.MigratedMenuShortcutsV1 == true);
    CHECK(cfg.Keybindings.FindBindingIndex("ui.zoom.in", "{}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("ui.zoom.out", "{}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("ui.zoom.reset", "{}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("ui.open_view", "{}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("grid.clear_selection", "{}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("view.toggle.log", "{\"action\":\"show\"}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("view.toggle.backend_audit", "{\"action\":\"show\"}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("view.toggle.source_annotate", "{\"action\":\"toggle\"}") >= 0);

    // The Ctrl+= → ui.zoom.in default binding (the marquee "Zoom In" symptom) is restored on its key.
    const int zi = cfg.Keybindings.FindBindingIndex("ui.zoom.in", "{}");
    REQUIRE(zi >= 0);
    CHECK(cfg.Keybindings.Bindings[static_cast<std::size_t>(zi)].Hotkey == "Ctrl+=");

    // Pre-existing bindings survive the seed (append-only — the migration never clobbers the table).
    CHECK(cfg.Keybindings.FindBindingIndex("app.dock_debug.toggle", "{}") >= 0);
    CHECK(cfg.Keybindings.FindBindingIndex("view.sidebar.primary", "{\"action\":\"toggle\"}") >= 0);
}

TEST_CASE("ConfigMigration menu shortcuts: respects a user rebind of a new command (no duplicate)") {
    smatchet_tests::TestEnvGuard env;
    // User already rebound Zoom In to Ctrl+Up in this build, then the migration lands. The
    // (command_id, args_json) identity matches the default, so the seed must NOT add a second
    // ui.zoom.in binding — the user's chosen key is preserved.
    const std::string pre = R"json({"tracker_type":"Jira","keybindings":{"bindings":[
        {"command_id":"ui.zoom.in","hotkey":"Ctrl+Up","args_json":"{}","enabled":true}
    ]}})json";
    WriteConfigRaw(env, pre);

    const TrackerConfig cfg = ConfigManager::Load();

    CHECK(cfg.MigratedMenuShortcutsV1 == true);
    CHECK(CountCommand(cfg.Keybindings, "ui.zoom.in") == 1); // not duplicated
    const int zi = cfg.Keybindings.FindBindingIndex("ui.zoom.in", "{}");
    REQUIRE(zi >= 0);
    CHECK(cfg.Keybindings.Bindings[static_cast<std::size_t>(zi)].Hotkey == "Ctrl+Up"); // user key kept
    // The other new defaults are still seeded alongside the preserved rebind.
    CHECK(cfg.Keybindings.FindBindingIndex("ui.zoom.out", "{}") >= 0);
}

TEST_CASE("ConfigMigration menu shortcuts: flag set on disk skips the seed (cleared stays cleared)") {
    smatchet_tests::TestEnvGuard env;
    // User on the current build cleared Zoom In then saved — migrated_menu_shortcuts_v1=true
    // persists. A reload must respect that choice and NOT resurrect ui.zoom.in.
    const std::string pre =
        R"json({"tracker_type":"Jira","migrated_menu_shortcuts_v1":true,"keybindings":{"bindings":[
        {"command_id":"view.sidebar.primary","hotkey":"Ctrl+B","args_json":"{\"action\":\"toggle\"}","enabled":true}
    ]}})json";
    WriteConfigRaw(env, pre);

    const TrackerConfig cfg = ConfigManager::Load();

    CHECK(cfg.MigratedMenuShortcutsV1 == true);
    CHECK(cfg.Keybindings.FindBindingIndex("ui.zoom.in", "{}") < 0); // not resurrected
}

TEST_CASE("ConfigMigration menu shortcuts: both views_dashboard arg variants seeded, idempotent on reload") {
    smatchet_tests::TestEnvGuard env;
    const std::string pre = R"json({"tracker_type":"Jira","keybindings":{"bindings":[
        {"command_id":"app.dock_debug.toggle","hotkey":"Ctrl+Alt+D","args_json":"{}","enabled":true}
    ]}})json";
    WriteConfigRaw(env, pre);

    const TrackerConfig first = ConfigManager::Load();
    // The shared views_dashboard command binds two distinct args → two distinct bindings, matched
    // by exact (command, args) identity.
    CHECK(first.Keybindings.FindBindingIndex("view.toggle.views_dashboard", "{\"action\":\"show\"}") >= 0);
    CHECK(first.Keybindings.FindBindingIndex("view.toggle.views_dashboard",
                                             "{\"action\":\"show\",\"via\":\"open_project_view\"}") >= 0);
    CHECK(CountCommand(first.Keybindings, "view.toggle.views_dashboard") == 2);

    // Persist (writes migrated_menu_shortcuts_v1=true + the full table) + reload — the seed is a
    // one-shot, so the binding count does not grow on the second Load.
    const std::size_t firstCount = first.Keybindings.Bindings.size();
    ConfigManager::Save(first);
    ConfigManager::InvalidateCache();
    const TrackerConfig second = ConfigManager::Load();
    CHECK(second.MigratedMenuShortcutsV1 == true);
    CHECK(second.Keybindings.Bindings.size() == firstCount); // no duplicate seed on reload
    CHECK(CountCommand(second.Keybindings, "view.toggle.views_dashboard") == 2);
}

// --- Quick-create issue keybinding — one-shot seed migration (migrated_quick_create_hotkey_v1) ---
//
// Same replace-not-merge failure class as the menu-shortcut seed above: a config saved before the
// quick-create popup existed carries a keybindings block without issue.quick_create.open, so the
// Ctrl+Shift+T default would never arrive without the one-shot seed.

TEST_CASE("ConfigMigration quick-create: seeds Ctrl+Shift+T into a pre-existing config") {
    smatchet_tests::TestEnvGuard env;
    const std::string pre = R"json({"tracker_type":"Jira","keybindings":{"bindings":[
        {"command_id":"view.sidebar.primary","hotkey":"Ctrl+B","args_json":"{\"action\":\"toggle\"}","enabled":true}
    ]}})json";
    WriteConfigRaw(env, pre);

    const TrackerConfig cfg = ConfigManager::Load();

    CHECK(cfg.MigratedQuickCreateHotkeyV1 == true);
    const int idx = cfg.Keybindings.FindBindingIndex("issue.quick_create.open", "{}");
    REQUIRE(idx >= 0);
    CHECK(cfg.Keybindings.Bindings[static_cast<std::size_t>(idx)].Hotkey == "Ctrl+Shift+T");
    // Pre-existing bindings survive (append-only).
    CHECK(cfg.Keybindings.FindBindingIndex("view.sidebar.primary", "{\"action\":\"toggle\"}") >= 0);
}

TEST_CASE("ConfigMigration quick-create: respects a user rebind (no duplicate, key kept)") {
    smatchet_tests::TestEnvGuard env;
    const std::string pre = R"json({"tracker_type":"Jira","keybindings":{"bindings":[
        {"command_id":"issue.quick_create.open","hotkey":"Ctrl+Shift+Q","args_json":"{}","enabled":true}
    ]}})json";
    WriteConfigRaw(env, pre);

    const TrackerConfig cfg = ConfigManager::Load();

    CHECK(cfg.MigratedQuickCreateHotkeyV1 == true);
    CHECK(CountCommand(cfg.Keybindings, "issue.quick_create.open") == 1);
    const int idx = cfg.Keybindings.FindBindingIndex("issue.quick_create.open", "{}");
    REQUIRE(idx >= 0);
    CHECK(cfg.Keybindings.Bindings[static_cast<std::size_t>(idx)].Hotkey == "Ctrl+Shift+Q");
}

TEST_CASE("ConfigMigration quick-create: flag set on disk skips the seed (cleared stays cleared)") {
    smatchet_tests::TestEnvGuard env;
    const std::string pre =
        R"json({"tracker_type":"Jira","migrated_quick_create_hotkey_v1":true,"keybindings":{"bindings":[
        {"command_id":"view.sidebar.primary","hotkey":"Ctrl+B","args_json":"{\"action\":\"toggle\"}","enabled":true}
    ]}})json";
    WriteConfigRaw(env, pre);

    const TrackerConfig cfg = ConfigManager::Load();

    CHECK(cfg.MigratedQuickCreateHotkeyV1 == true);
    CHECK(cfg.Keybindings.FindBindingIndex("issue.quick_create.open", "{}") < 0); // not resurrected
}
