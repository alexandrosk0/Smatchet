// Regression net for DR3 — a single type-mismatched scalar key in the on-disk config must not
// drop the stored secrets (or list fields) on Load, and must therefore not let a subsequent Save
// permanently wipe the user's tokens.
//
// Before the fix, ConfigManager::Load wrapped every field-group loader in ONE try/catch, so a bad
// scalar (a string where an int is expected) threw before the secret loader ran, leaving the
// secrets empty. A later Save then persisted those empty secrets, erasing the plaintext fallback.
// The fix loads each group under its own try/catch; these cases lock that behavior in.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

namespace {

// Overwrite the guard's minimal config with a payload that carries a valid plaintext secret and a
// list field alongside one type-mismatched scalar (window_x holds a string, not an int).
void WriteHostileConfig(const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << "{\"read_only_mode\":false,"
         "\"token\":\"regress-secret-token-ABC\","
         "\"window_x\":\"not-a-number\","
         "\"mcp_export_fields\":[\"status\",\"priority\"]}";
    f.close();
}

} // namespace

TEST_CASE("ConfigManager::Load keeps secrets and lists when one scalar has the wrong type") {
    smatchet_tests::TestEnvGuard env;

    WriteHostileConfig(ConfigManager::GetConfigPath());
    ConfigManager::InvalidateCache();

    const TrackerConfig cfg = ConfigManager::Load();

    // The mistyped scalar must not have aborted the secret group. On non-Win32 the plaintext
    // `token` is read directly; on Win32 the loader falls back to the plaintext when no encrypted
    // key is present, so this holds on every platform.
    CHECK(cfg.ApiToken == "regress-secret-token-ABC");

    // The list group runs after the secret group and must likewise survive the bad scalar.
    const std::vector<std::string> expected = {"status", "priority"};
    CHECK(cfg.McpExportFields == expected);
}

TEST_CASE("ConfigManager Save after a bad-scalar Load does not wipe the stored secret") {
    smatchet_tests::TestEnvGuard env;

    WriteHostileConfig(ConfigManager::GetConfigPath());
    ConfigManager::InvalidateCache();

    // Load recovers the secret despite the bad scalar, then a Save persists the recovered config.
    const TrackerConfig loaded = ConfigManager::Load();
    REQUIRE(loaded.ApiToken == "regress-secret-token-ABC");
    ConfigManager::Save(loaded);

    // A fresh Load from the just-written file still sees the secret — proving the round-trip did
    // not erase it.
    ConfigManager::InvalidateCache();
    const TrackerConfig reloaded = ConfigManager::Load();
    CHECK(reloaded.ApiToken == "regress-secret-token-ABC");
}
