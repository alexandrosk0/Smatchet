// ConfigManager pure-logic tests — drives the public statics that do not require
// a populated config file: directory normalization, redirect/restore, env-var
// precedence, and fresh-install defaults.
//
// Migration cases that require fixture JSON live in ConfigMigration.test.cpp.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

TEST_CASE("ConfigManager::SetUserDataDirectory normalizes backslashes and trailing separator") {
    // Backslashes are converted to forward slashes; missing trailing slash is appended.
    ConfigManager::SetUserDataDirectory("C:\\Users\\dev\\AppData\\Smatchet");
    const std::string& dir = ConfigManager::GetUserDataDirectory();
    CHECK(dir == "C:/Users/dev/AppData/Smatchet/");

    // Already-normalized input is idempotent — re-set yields the same shape.
    ConfigManager::SetUserDataDirectory("C:/Users/dev/AppData/Smatchet/");
    CHECK(ConfigManager::GetUserDataDirectory() == "C:/Users/dev/AppData/Smatchet/");

    // POSIX-style path with no separator gets one appended.
    ConfigManager::SetUserDataDirectory("/var/lib/smatchet");
    CHECK(ConfigManager::GetUserDataDirectory() == "/var/lib/smatchet/");

    // Mixed-separator path is normalized to forward slashes throughout.
    ConfigManager::SetUserDataDirectory("/var\\lib/smatchet\\sub");
    CHECK(ConfigManager::GetUserDataDirectory() == "/var/lib/smatchet/sub/");

    // Clear for hermetic teardown.
    ConfigManager::SetUserDataDirectory("");
    CHECK(ConfigManager::GetUserDataDirectory().empty());
    ConfigManager::InvalidateCache();
}

TEST_CASE("ConfigManager path getters compose against the user-data directory") {
    smatchet_tests::TestEnvGuard env;
    // Use the normalized (forward-slash) directory the ConfigManager sees, not the raw temp
    // dir captured by TestEnvGuard (which may contain backslashes on Win32).
    const std::string base = ConfigManager::GetUserDataDirectory();
    REQUIRE_FALSE(base.empty());

    // Config / views / imgui-settings paths all live under the redirected user-data dir.
    const std::string cfgPath = ConfigManager::GetConfigPath();
    CHECK(cfgPath.find(base) == 0);
    CHECK(cfgPath.substr(base.size()) == "smatchet_config.json");

    const std::string viewsPath = ConfigManager::GetViewsPath();
    CHECK(viewsPath.find(base) == 0);
    CHECK(viewsPath.substr(base.size()) == "smatchet_views.json");

    const std::string imguiPath = ConfigManager::GetImGuiSettingsPath();
    CHECK(imguiPath.find(base) == 0);
    CHECK(imguiPath.substr(base.size()) == "imgui.ini");
}

TEST_CASE("ConfigManager::GetFilesBaseDirectory falls back to runtime asset directory when user-data is empty") {
    // Reset both ref strings; then prove that GetFilesBaseDirectory returns runtime-asset when
    // user-data is empty (legacy single-base portable mode).
    ConfigManager::SetUserDataDirectory("");
    ConfigManager::SetRuntimeAssetDirectory("/runtime/asset/dir");

    const std::string& filesBase = ConfigManager::GetFilesBaseDirectory();
    CHECK(filesBase == "/runtime/asset/dir/");

    // Restoring user-data takes precedence.
    ConfigManager::SetUserDataDirectory("/explicit/user/data");
    CHECK(ConfigManager::GetUserDataDirectory() == "/explicit/user/data/");

    // Tidy up.
    ConfigManager::SetUserDataDirectory("");
    ConfigManager::SetRuntimeAssetDirectory("");
}

TEST_CASE("ConfigManager::SetBaseDirectoryForFiles sets both runtime + user-data") {
    // Legacy combined entrypoint: one call points both refs at the same directory.
    ConfigManager::SetBaseDirectoryForFiles("D:\\portable\\smatchet");
    CHECK(ConfigManager::GetUserDataDirectory() == "D:/portable/smatchet/");
    CHECK(ConfigManager::GetRuntimeAssetDirectory() == "D:/portable/smatchet/");

    // Empty input clears both.
    ConfigManager::SetBaseDirectoryForFiles("");
    CHECK(ConfigManager::GetUserDataDirectory().empty());
    CHECK(ConfigManager::GetRuntimeAssetDirectory().empty());
}

TEST_CASE("ConfigManager::EnsureDefaultImGuiSettingsFile creates imgui.ini under redirected dir") {
    smatchet_tests::TestEnvGuard env;
    const std::string imguiPath = ConfigManager::GetImGuiSettingsPath();
    // Ensure clean slate.
    std::remove(imguiPath.c_str());

    ConfigManager::EnsureDefaultImGuiSettingsFile();
    std::ifstream f(imguiPath, std::ios::binary);
    CHECK(f.good());

    // The embedded default ini contains a DockSpace section header.
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK(contents.find("[Docking][Data]") != std::string::npos);

    // Second call is a no-op — file is not overwritten / mtime not bumped — but we just check
    // it still exists.
    ConfigManager::EnsureDefaultImGuiSettingsFile();
    std::ifstream f2(imguiPath, std::ios::binary);
    CHECK(f2.good());
    f2.close();
    std::remove(imguiPath.c_str());
}

TEST_CASE("ConfigManager::NormalizeUiLanguageCode canonicalizes locale aliases") {
    using N = std::string (*)(const std::string&);
    N norm = &ConfigManager::NormalizeUiLanguageCode;

    CHECK(norm("fr") == "fr-FR");
    CHECK(norm("FR") == "fr-FR");
    CHECK(norm("fr-FR") == "fr-FR");
    CHECK(norm("fr_FR") == "fr-FR");
    CHECK(norm(" fr ") == "fr-FR");

    CHECK(norm("en") == "en-US");
    CHECK(norm("en-US") == "en-US");
    CHECK(norm("") == "en-US");
    CHECK(norm("xx-YY") == "en-US"); // unknown locale falls back to en-US default.
}

TEST_CASE("ConfigManager fresh-install defaults: ReadOnlyMode=true when no config file exists") {
    // Hostile fresh-install scenario — point ConfigManager at an empty directory with no
    // smatchet_config.json, then Load(). The "first-launch protection" should kick in:
    // ReadOnlyMode flips to true even though TrackerConfig's struct default is false.
    const char* envTmp = nullptr;
#if defined(_WIN32)
    envTmp = std::getenv("TEMP");
    if (!envTmp)
        envTmp = std::getenv("TMP");
    if (!envTmp)
        envTmp = "C:\\Windows\\Temp";
    const char sep = '\\';
#else
    envTmp = std::getenv("TMPDIR");
    if (!envTmp)
        envTmp = "/tmp";
    const char sep = '/';
#endif
    std::string emptyDir = std::string(envTmp) + sep + "smatchet_freshinstall_test";
#if defined(_WIN32)
    ::_mkdir(emptyDir.c_str());
#else
    ::mkdir(emptyDir.c_str(), 0755);
#endif
    if (emptyDir.back() != sep)
        emptyDir.push_back(sep);

    ConfigManager::SetUserDataDirectory(emptyDir);
    ConfigManager::InvalidateCache();
    // Ensure no leftover config from a previous run.
    std::remove((emptyDir + "smatchet_config.json").c_str());

    const TrackerConfig cfg = ConfigManager::Load();
    CHECK(cfg.ReadOnlyMode == true);
    // ShowPreferencesWindow also flips on for fresh-install to surface tracker setup.
    CHECK(cfg.ShowPreferencesWindow == true);

    // With a config file present (read_only_mode explicitly false), the override is gone.
    const std::string cfgPath = emptyDir + "smatchet_config.json";
    {
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":false}";
    }
    ConfigManager::InvalidateCache();
    const TrackerConfig cfgWithFile = ConfigManager::Load();
    CHECK(cfgWithFile.ReadOnlyMode == false);

    // Tidy up.
    std::remove(cfgPath.c_str());
    ConfigManager::SetUserDataDirectory("");
    ConfigManager::InvalidateCache();
#if defined(_WIN32)
    ::_rmdir(emptyDir.c_str());
#else
    ::rmdir(emptyDir.c_str());
#endif
}

TEST_CASE("ConfigManager SMATCHET_DB_PATH env var overrides db_path on Load") {
    smatchet_tests::TestEnvGuard env;

    // Baseline: env var unset → db_path comes from the minimal config (which doesn't set it,
    // so defaults to SmatchetDefaults::kDefaultDbPath).
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_DB_PATH", "");
#else
    ::unsetenv("SMATCHET_DB_PATH");
#endif
    ConfigManager::InvalidateCache();
    const TrackerConfig cfgNoEnv = ConfigManager::Load();
    const std::string baselineDbPath = cfgNoEnv.DbPath;
    CHECK_FALSE(baselineDbPath.empty()); // defaults apply

    // With env set, Load() applies the override (post-disk-read, pre-cache).
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_DB_PATH", "C:/override/path/db.sqlite");
#else
    ::setenv("SMATCHET_DB_PATH", "/override/path/db.sqlite", 1);
#endif
    ConfigManager::InvalidateCache();
    const TrackerConfig cfgWithEnv = ConfigManager::Load();
#if defined(_WIN32)
    CHECK(cfgWithEnv.DbPath == "C:/override/path/db.sqlite");
#else
    CHECK(cfgWithEnv.DbPath == "/override/path/db.sqlite");
#endif

    // Cleanup.
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_DB_PATH", "");
#else
    ::unsetenv("SMATCHET_DB_PATH");
#endif
    ConfigManager::InvalidateCache();
}

TEST_CASE("ConfigManager CLI overrides win over env vars on Load") {
    smatchet_tests::TestEnvGuard env;

#if defined(_WIN32)
    ::_putenv_s("SMATCHET_MCP_PORT", "9999");
#else
    ::setenv("SMATCHET_MCP_PORT", "9999", 1);
#endif

    ConfigManager::CliOverrides cli;
    cli.HasMcpPort = true;
    cli.McpPort = 12345;

    // CLI overrides bypass the cache (canUseCache=false when any override is set), so Load()
    // re-reads disk + re-applies overrides every time.
    ConfigManager::InvalidateCache();
    const TrackerConfig cfg = ConfigManager::Load(cli);
    CHECK(cfg.McpPort == 12345); // CLI wins; env var (9999) is overridden.

    // Without CLI, env wins.
    ConfigManager::InvalidateCache();
    const TrackerConfig cfgNoCli = ConfigManager::Load();
    CHECK(cfgNoCli.McpPort == 9999);

    // Cleanup.
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_MCP_PORT", "");
#else
    ::unsetenv("SMATCHET_MCP_PORT");
#endif
    ConfigManager::InvalidateCache();
}
