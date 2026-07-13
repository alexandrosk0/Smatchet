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

TEST_CASE("ConfigManager::SetPlatformSharedUserDataDirectoryOverride wins over OS resolution") {
    // Capture whatever the OS resolver yields on this platform (may be empty on a degraded
    // env). The override must take precedence over this baseline and clearing must restore it,
    // so the assertion is hermetic regardless of which platform the test runs on.
    const std::string baseline = ConfigManager::GetPlatformSharedUserDataDirectory();

    // Inject a host-resolved path (mirrors the Android host supplying its app-private dir).
    // Backslashes are normalized and a trailing separator is appended, like every other setter.
    ConfigManager::SetPlatformSharedUserDataDirectoryOverride("C:\\android\\app\\data");
    CHECK(ConfigManager::GetPlatformSharedUserDataDirectory() == "C:/android/app/data/");

    // Re-injecting an already-normalized path is idempotent.
    ConfigManager::SetPlatformSharedUserDataDirectoryOverride("/data/data/com.smatchet/files/");
    CHECK(ConfigManager::GetPlatformSharedUserDataDirectory() == "/data/data/com.smatchet/files/");

    // Empty string clears the override — OS resolution resumes and we get the baseline back.
    ConfigManager::SetPlatformSharedUserDataDirectoryOverride("");
    CHECK(ConfigManager::GetPlatformSharedUserDataDirectory() == baseline);
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

TEST_CASE("ConfigManager SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK env override") {
    smatchet_tests::TestEnvGuard env;

    // The CI --spawn workflows USED to export SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK
    // =false at job scope (PR A / #1566 fast-unblock); that opt-out was removed once
    // the spawn token handshake landed (PR B / #1576). Clear the var explicitly
    // anyway before the "default ON" assertion so we test the real compiled default
    // regardless of any ambient (developer-local or re-introduced) override.
    // (TestEnvGuard does NOT touch this env var — the final Cleanup block below is
    // what restores a clean environment for later cases.)
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "");
#else
    ::unsetenv("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK");
#endif

    // Default ON (#1566 security hardening): without the env var the loopback
    // token requirement stays true.
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().McpRequireTokenOnLoopback == true);

    // env "false" disables it — the CI --spawn unblock path. This flag gates
    // UNAUTHENTICATED loopback MCP access, so it parses fail-closed: only an
    // explicit "false"/"0" disables it.
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "false");
#else
    ::setenv("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "false", 1);
#endif
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().McpRequireTokenOnLoopback == false);

    // env "true" keeps it on.
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "true");
#else
    ::setenv("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "true", 1);
#endif
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().McpRequireTokenOnLoopback == true);

    // Fail-closed: a malformed value is NOT treated as "false" — it preserves
    // the secure default (true). Locks the #1566 hardening against a typo
    // silently dropping the loopback token requirement.
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "garbage");
#else
    ::setenv("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "garbage", 1);
#endif
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().McpRequireTokenOnLoopback == true);

    // Cleanup.
#if defined(_WIN32)
    ::_putenv_s("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK", "");
#else
    ::unsetenv("SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK");
#endif
    ConfigManager::InvalidateCache();
}

// --- Save/Load round-trip regression net for the field-registration-table refactor. -------
//
// decompose-top-20-monoliths § ConfigManager::Load/Save. These are the
// real safety net: ConfigManager::Save/Load is the config boot path; a dropped or mismapped
// field in the table would silently corrupt user config. Each persisted TrackerConfig field is
// written to a NON-default value, Saved through the real ConfigManager seam, reloaded, and the
// read-back asserted equal. The defaults case proves a missing/empty config yields struct
// defaults; the secret case proves DPAPI-at-rest (Win32) + round-trip.

namespace {

// Hostile-non-default values chosen inside every documented clamp range so the round-trip is
// exact. `summary` is intentionally avoided in inherit lists (Save filters it out).
TrackerConfig MakeNonDefaultConfig() {
    TrackerConfig c;
    c.Domain = "rt.example.atlassian.net";
    c.Email = "rt-user@example.com";
    c.ApiToken = "rt-jira-token-zzz";
    c.TrackerType = "Plane";
    c.PlaneUrl = "https://api.plane.example";
    c.PlaneWorkspaceSlug = "rt-workspace";
    c.PlaneApiKey = "rt-plane-key-zzz";
    c.GitHubPat = "rt-github-pat-zzz";
    c.GitHubBaseUrl = "https://gh.example/api/v3";
    c.GitHubOwner = "rt-owner";
    c.GitHubRepo = "rt-repo";
    c.LinearApiKey = "rt-linear-key-zzz";
    c.LinearBaseUrl = "https://linear.example/graphql";
    c.LinearTeamId = "rt-team-uuid";
    c.LinearTeamKey = "RTK";
    c.LinearWorkspaceUrl = "https://linear.app/rt-workspace";
    c.BugReportRelayUrl = "https://relay.example";
    c.BugReportRelayKey = "rt-relay-key";
    c.BugReportGitHubOwner = "rt-bug-owner";
    c.BugReportGitHubRepo = "rt-bug-repo";
    c.BugReportGitHubBaseUrl = "https://bug.example/api/v3";
    c.BugReportAssetsRepo = "rt-owner/rt-assets";
    c.BugReportPersistPat = true; // must be true for BugReportGitHubPat to persist
    c.BugReportGitHubPat = "rt-bugreport-pat-zzz";
    c.BugReportHotkey = "Ctrl+Alt+J";
    c.BugReportHotkeyEnabled = false;
    c.BugReportScreenshotDefault = false;
    c.JqlQuery = "project = RT";
    c.EnableFieldOverflowTooltips = false;
    c.SingleClickToEditGridCells = false;
    c.DefaultLongTextEditorPreview = true;
    c.ReadOnlyMode = true;
    c.BackendHasBeenReachable = true;
    c.GridEndWheelSwallowsBeforeHorizontal = 7;
    c.WindowX = 11;
    c.WindowY = 22;
    c.WindowWidth = 1600;
    c.WindowHeight = 900;
    c.WindowMaximized = true;
    c.FieldCatalogCacheMaxProjects = 24;
    c.ShowPreferencesWindow = true;
    c.ShowViewsDashboardWindow = false;
    c.ShowPerformanceWindow = true;
    c.ShowLogWindow = true;
    c.VsyncEnabled = false; // default true — flip to prove the round-trip
    c.LogMinLevel = "debug";
    c.LogTrackerHttpBodies = true;
    c.LogP4Io = true;
    c.McpEnabled = true;
    c.McpPort = 5599;
    c.McpAllowRemote = true;
    c.McpAuthToken = "rt-mcp-token-zzz";
    c.McpAllowLuaExecution = true;
    c.McpRequireTokenOnLoopback = false; // default true — flip to prove the round-trip
    c.McpExportFields = {"status", "priority"};
    c.ShowMcpServerWindow = true;
    c.McpServerInfoPanelHeightPx = 123.0f;
    c.McpServerActivityPanelHeightPx = 234.0f;
    c.AnnotateAllowCustomCommands = true;
    c.DefaultIssueTypeId = "10042";
    c.DefaultIssueTypeName = "Bug";
    c.ImportMaxConcurrent = 9;
    c.LastImportDirectory = "/rt/import";
    c.LastExportDirectory = "/rt/export";
    c.NewIssueInheritFieldIds = {"issuetype", "labels", "priority"};
    c.NewIssueInheritFieldIdsPlane = {"issuetype", "state"};
    c.NewIssueInheritFieldIdsGitHub = {"issuetype", "assignees"};
    c.NewIssueInheritFieldIdsLinear = {"priority", "labels"};
    c.MigratedInheritIssueTypeV1 = true; // avoid the one-shot issuetype injection on reload
    c.DurationSuggestions = {"5m", "45m"};
    c.WorkLogCommentTemplates = {"rt template one", "rt template two"};
    c.DateFormatOption = "absolute";
    c.DateCompactRelativeThresholdDays = 30;
    c.ViewFieldPickerHeight = 333.0f;
    c.ViewsSidebarWidth = 199.0f;
    c.ViewsFieldsSplitRatio = 0.42f;
    c.ShowPrimarySideBar = false;
    c.ShowSecondarySideBar = true;
    c.ShowPanel = false;
    c.ShowStatusBar = false;
    c.Density = TrackerConfig::UiDensity::Compact;
    c.PanelDockSide = TrackerConfig::PanelPosition::Right;
    c.PrimarySideBarOnRight = false;
    c.AiProviderKind = 1;
    c.AiApiKey = "rt-ai-openai-zzz";
    c.AiAnthropicApiKey = "rt-ai-anthropic-zzz";
    c.AiDeepSeekApiKey = "rt-ai-deepseek-zzz";
    c.AiOllamaBaseUrl = "http://localhost:11999";
    c.AiBaseUrl = "https://ai.example";
    c.AiModelOpenAi = "gpt-rt";
    c.AiModelAnthropic = "claude-rt";
    c.AiModelOllama = "llama-rt";
    c.AiDeepSeekBaseUrl = "https://deepseek.example";
    c.AiModelDeepSeek = "deepseek-rt";
    c.AiReasoningEffort = "high";
    c.AssistantPanelOpen = true;
    c.AssistantPanelWidth = 412.0f;
    c.AssistantPanelOnSecondarySide = true;
    c.AgentsMdGlobalPath = "/rt/agents.md";
    c.ProjectAgentsMdPath = "/rt/project/AGENTS.md";
    c.AgentsMdAutoDiscoverProject = true;
    c.AssistantContextBlockSelection = false;
    c.AssistantContextBlockVisibleRows = false;
    c.AssistantContextBlockActiveTicket = false;
    c.AssistantContextBlockActiveView = false;
    c.AssistantContextBlockAuditTrail = true;
    c.AssistantHistoryMaxRows = 777;
    c.AiPrefsVerifyOnSave = false;
    c.LayoutSchemaVersion = 4;
    c.SelectedFontName = "Consolas";
    c.FontSizePt = 20;
    c.Theme = ThemeId::NortonCommander;
    c.UiLanguage = "fr-FR";
    c.UpdateCheckEnabled = false;
    c.UpdateIncludePrerelease = true;
    c.UpdateSkipVersion = "9.9.9";
    c.UpdateGithubRepo = "rt-owner/rt-fork";
    c.TicketChangeMonitorEnabled = false;   // default true — flip to prove the round-trip
    c.TicketChangeMonitorIntervalSec = 300; // inside the [30, 3600] clamp band
    // Quick-create context toggles default true — flip a spread to prove the round-trip.
    c.QuickCreateCtxEngineVersion = false;
    c.QuickCreateCtxProject = false;
    c.QuickCreateCtxPlatform = false;
    c.QuickCreateCtxLevel = false;
    c.QuickCreateCtxPieState = false;
    c.QuickCreateCtxSelectedActors = false;
    c.QuickCreateCtxLogTail = false;
    c.QuickCreateCtxLogLines = 120; // inside the [1, 300] clamp band
    return c;
}

} // namespace

TEST_CASE("ConfigManager Save/Load per-field round-trip preserves every persisted field") {
    smatchet_tests::TestEnvGuard env;

    const TrackerConfig in = MakeNonDefaultConfig();
    ConfigManager::Save(in);
    ConfigManager::InvalidateCache();
    const TrackerConfig out = ConfigManager::Load();

    // Strings.
    CHECK(out.Domain == in.Domain);
    CHECK(out.Email == in.Email);
    CHECK(out.ApiToken == in.ApiToken);
    CHECK(out.TrackerType == in.TrackerType);
    CHECK(out.PlaneUrl == in.PlaneUrl);
    CHECK(out.PlaneWorkspaceSlug == in.PlaneWorkspaceSlug);
    CHECK(out.PlaneApiKey == in.PlaneApiKey);
    CHECK(out.GitHubPat == in.GitHubPat);
    CHECK(out.GitHubBaseUrl == in.GitHubBaseUrl);
    CHECK(out.GitHubOwner == in.GitHubOwner);
    CHECK(out.GitHubRepo == in.GitHubRepo);
    CHECK(out.LinearApiKey == in.LinearApiKey);
    CHECK(out.LinearBaseUrl == in.LinearBaseUrl);
    CHECK(out.LinearTeamId == in.LinearTeamId);
    CHECK(out.LinearTeamKey == in.LinearTeamKey);
    CHECK(out.LinearWorkspaceUrl == in.LinearWorkspaceUrl);
    CHECK(out.BugReportRelayUrl == in.BugReportRelayUrl);
    CHECK(out.BugReportRelayKey == in.BugReportRelayKey);
    CHECK(out.BugReportGitHubOwner == in.BugReportGitHubOwner);
    CHECK(out.BugReportGitHubRepo == in.BugReportGitHubRepo);
    CHECK(out.BugReportGitHubBaseUrl == in.BugReportGitHubBaseUrl);
    CHECK(out.BugReportAssetsRepo == in.BugReportAssetsRepo);
    CHECK(out.BugReportGitHubPat == in.BugReportGitHubPat);
    CHECK(out.BugReportHotkey == in.BugReportHotkey);
    CHECK(out.JqlQuery == in.JqlQuery);
    CHECK(out.LogMinLevel == in.LogMinLevel);
    CHECK(out.McpAuthToken == in.McpAuthToken);
    CHECK(out.DefaultIssueTypeId == in.DefaultIssueTypeId);
    CHECK(out.DefaultIssueTypeName == in.DefaultIssueTypeName);
    CHECK(out.LastImportDirectory == in.LastImportDirectory);
    CHECK(out.LastExportDirectory == in.LastExportDirectory);
    CHECK(out.DateFormatOption == in.DateFormatOption);
    CHECK(out.AiApiKey == in.AiApiKey);
    CHECK(out.AiAnthropicApiKey == in.AiAnthropicApiKey);
    CHECK(out.AiDeepSeekApiKey == in.AiDeepSeekApiKey);
    CHECK(out.AiOllamaBaseUrl == in.AiOllamaBaseUrl);
    CHECK(out.AiBaseUrl == in.AiBaseUrl);
    CHECK(out.AiModelOpenAi == in.AiModelOpenAi);
    CHECK(out.AiModelAnthropic == in.AiModelAnthropic);
    CHECK(out.AiModelOllama == in.AiModelOllama);
    CHECK(out.AiDeepSeekBaseUrl == in.AiDeepSeekBaseUrl);
    CHECK(out.AiModelDeepSeek == in.AiModelDeepSeek);
    CHECK(out.AiReasoningEffort == in.AiReasoningEffort);
    CHECK(out.AgentsMdGlobalPath == in.AgentsMdGlobalPath);
    CHECK(out.ProjectAgentsMdPath == in.ProjectAgentsMdPath);
    CHECK(out.SelectedFontName == in.SelectedFontName);
    CHECK(out.UiLanguage == in.UiLanguage);
    CHECK(out.UpdateSkipVersion == in.UpdateSkipVersion);
    CHECK(out.UpdateGithubRepo == in.UpdateGithubRepo);

    // Bools.
    CHECK(out.BugReportPersistPat == in.BugReportPersistPat);
    CHECK(out.BugReportHotkeyEnabled == in.BugReportHotkeyEnabled);
    CHECK(out.BugReportScreenshotDefault == in.BugReportScreenshotDefault);
    CHECK(out.EnableFieldOverflowTooltips == in.EnableFieldOverflowTooltips);
    CHECK(out.SingleClickToEditGridCells == in.SingleClickToEditGridCells);
    CHECK(out.DefaultLongTextEditorPreview == in.DefaultLongTextEditorPreview);
    CHECK(out.ReadOnlyMode == in.ReadOnlyMode);
    CHECK(out.BackendHasBeenReachable == in.BackendHasBeenReachable);
    CHECK(out.WindowMaximized == in.WindowMaximized);
    CHECK(out.ShowPreferencesWindow == in.ShowPreferencesWindow);
    CHECK(out.ShowViewsDashboardWindow == in.ShowViewsDashboardWindow);
    CHECK(out.ShowPerformanceWindow == in.ShowPerformanceWindow);
    CHECK(out.ShowLogWindow == in.ShowLogWindow);
    CHECK(out.VsyncEnabled == in.VsyncEnabled);
    CHECK(out.LogTrackerHttpBodies == in.LogTrackerHttpBodies);
    CHECK(out.LogP4Io == in.LogP4Io);
    CHECK(out.McpEnabled == in.McpEnabled);
    CHECK(out.McpAllowRemote == in.McpAllowRemote);
    CHECK(out.McpAllowLuaExecution == in.McpAllowLuaExecution);
    CHECK(out.McpRequireTokenOnLoopback == in.McpRequireTokenOnLoopback);
    CHECK(out.ShowMcpServerWindow == in.ShowMcpServerWindow);
    CHECK(out.AnnotateAllowCustomCommands == in.AnnotateAllowCustomCommands);
    CHECK(out.AssistantPanelOpen == in.AssistantPanelOpen);
    CHECK(out.AssistantPanelOnSecondarySide == in.AssistantPanelOnSecondarySide);
    CHECK(out.AgentsMdAutoDiscoverProject == in.AgentsMdAutoDiscoverProject);
    CHECK(out.AssistantContextBlockSelection == in.AssistantContextBlockSelection);
    CHECK(out.AssistantContextBlockVisibleRows == in.AssistantContextBlockVisibleRows);
    CHECK(out.AssistantContextBlockActiveTicket == in.AssistantContextBlockActiveTicket);
    CHECK(out.AssistantContextBlockActiveView == in.AssistantContextBlockActiveView);
    CHECK(out.AssistantContextBlockAuditTrail == in.AssistantContextBlockAuditTrail);
    CHECK(out.AiPrefsVerifyOnSave == in.AiPrefsVerifyOnSave);
    CHECK(out.ShowPrimarySideBar == in.ShowPrimarySideBar);
    CHECK(out.ShowSecondarySideBar == in.ShowSecondarySideBar);
    CHECK(out.ShowPanel == in.ShowPanel);
    CHECK(out.ShowStatusBar == in.ShowStatusBar);
    CHECK(out.PrimarySideBarOnRight == in.PrimarySideBarOnRight);
    CHECK(out.UpdateCheckEnabled == in.UpdateCheckEnabled);
    CHECK(out.UpdateIncludePrerelease == in.UpdateIncludePrerelease);
    CHECK(out.TicketChangeMonitorEnabled == in.TicketChangeMonitorEnabled);
    CHECK(out.QuickCreateCtxEngineVersion == in.QuickCreateCtxEngineVersion);
    CHECK(out.QuickCreateCtxProject == in.QuickCreateCtxProject);
    CHECK(out.QuickCreateCtxPlatform == in.QuickCreateCtxPlatform);
    CHECK(out.QuickCreateCtxLevel == in.QuickCreateCtxLevel);
    CHECK(out.QuickCreateCtxPieState == in.QuickCreateCtxPieState);
    CHECK(out.QuickCreateCtxSelectedActors == in.QuickCreateCtxSelectedActors);
    CHECK(out.QuickCreateCtxLogTail == in.QuickCreateCtxLogTail);

    // Ints.
    CHECK(out.GridEndWheelSwallowsBeforeHorizontal == in.GridEndWheelSwallowsBeforeHorizontal);
    CHECK(out.WindowX == in.WindowX);
    CHECK(out.WindowY == in.WindowY);
    CHECK(out.WindowWidth == in.WindowWidth);
    CHECK(out.WindowHeight == in.WindowHeight);
    CHECK(out.FieldCatalogCacheMaxProjects == in.FieldCatalogCacheMaxProjects);
    CHECK(out.McpPort == in.McpPort);
    CHECK(out.DateCompactRelativeThresholdDays == in.DateCompactRelativeThresholdDays);
    CHECK(out.ImportMaxConcurrent == in.ImportMaxConcurrent);
    CHECK(out.AiProviderKind == in.AiProviderKind);
    CHECK(out.AssistantHistoryMaxRows == in.AssistantHistoryMaxRows);
    CHECK(out.LayoutSchemaVersion == in.LayoutSchemaVersion);
    CHECK(out.FontSizePt == in.FontSizePt);
    CHECK(out.TicketChangeMonitorIntervalSec == in.TicketChangeMonitorIntervalSec);
    CHECK(out.QuickCreateCtxLogLines == in.QuickCreateCtxLogLines);

    // Floats.
    CHECK(out.McpServerInfoPanelHeightPx == doctest::Approx(in.McpServerInfoPanelHeightPx));
    CHECK(out.McpServerActivityPanelHeightPx == doctest::Approx(in.McpServerActivityPanelHeightPx));
    CHECK(out.AssistantPanelWidth == doctest::Approx(in.AssistantPanelWidth));
    CHECK(out.ViewFieldPickerHeight == doctest::Approx(in.ViewFieldPickerHeight));
    CHECK(out.ViewsSidebarWidth == doctest::Approx(in.ViewsSidebarWidth));
    CHECK(out.ViewsFieldsSplitRatio == doctest::Approx(in.ViewsFieldsSplitRatio));

    // Enums.
    CHECK(out.Density == in.Density);
    CHECK(out.PanelDockSide == in.PanelDockSide);
    CHECK(out.Theme == in.Theme);

    // Lists.
    CHECK(out.McpExportFields == in.McpExportFields);
    CHECK(out.NewIssueInheritFieldIds == in.NewIssueInheritFieldIds);
    CHECK(out.NewIssueInheritFieldIdsPlane == in.NewIssueInheritFieldIdsPlane);
    CHECK(out.NewIssueInheritFieldIdsGitHub == in.NewIssueInheritFieldIdsGitHub);
    CHECK(out.NewIssueInheritFieldIdsLinear == in.NewIssueInheritFieldIdsLinear);
    CHECK(out.DurationSuggestions == in.DurationSuggestions);
    CHECK(out.WorkLogCommentTemplates == in.WorkLogCommentTemplates);
    CHECK(out.MigratedInheritIssueTypeV1 == in.MigratedInheritIssueTypeV1);
}

TEST_CASE("ConfigManager Load of an empty config yields documented struct defaults") {
    smatchet_tests::TestEnvGuard env; // writes a minimal {"read_only_mode":false} config

    ConfigManager::InvalidateCache();
    const TrackerConfig cfg = ConfigManager::Load();
    const TrackerConfig def; // construct-time defaults

    CHECK(cfg.TrackerType == def.TrackerType);
    CHECK(cfg.JqlQuery == def.JqlQuery);
    CHECK(cfg.EnableFieldOverflowTooltips == def.EnableFieldOverflowTooltips);
    CHECK(cfg.SingleClickToEditGridCells == def.SingleClickToEditGridCells);
    CHECK(cfg.WindowWidth == def.WindowWidth);
    CHECK(cfg.WindowHeight == def.WindowHeight);
    CHECK(cfg.FieldCatalogCacheMaxProjects == def.FieldCatalogCacheMaxProjects);
    CHECK(cfg.LogMinLevel == def.LogMinLevel);
    CHECK(cfg.McpPort == def.McpPort);
    CHECK(cfg.ImportMaxConcurrent == def.ImportMaxConcurrent);
    CHECK(cfg.FontSizePt == def.FontSizePt);
    CHECK(cfg.Theme == def.Theme);
    CHECK(cfg.UiLanguage == def.UiLanguage);
    CHECK(cfg.Density == def.Density);
    CHECK(cfg.PanelDockSide == def.PanelDockSide);
    CHECK(cfg.AiModelOpenAi == def.AiModelOpenAi);
    CHECK(cfg.AiModelAnthropic == def.AiModelAnthropic);
    CHECK(cfg.AiReasoningEffort == def.AiReasoningEffort);
    CHECK(cfg.AssistantHistoryMaxRows == def.AssistantHistoryMaxRows);
    CHECK(cfg.UpdateGithubRepo == def.UpdateGithubRepo);
    CHECK(cfg.BugReportHotkey == def.BugReportHotkey);
    // Secrets default to empty when absent.
    CHECK(cfg.ApiToken.empty());
    CHECK(cfg.PlaneApiKey.empty());
    CHECK(cfg.GitHubPat.empty());
    CHECK(cfg.McpAuthToken.empty());
    CHECK(cfg.AiApiKey.empty());
}

TEST_CASE("ConfigManager secret round-trips and is stored encrypted-at-rest on Win32") {
    smatchet_tests::TestEnvGuard env;

    TrackerConfig in;
    const std::string secret = "super-secret-plane-key-RoundTrip-1234567890";
    in.PlaneApiKey = secret;
    in.TrackerType = "Plane";
    ConfigManager::Save(in);

    // Read the raw on-disk config bytes.
    const std::string cfgPath = ConfigManager::GetConfigPath();
    std::ifstream f(cfgPath, std::ios::binary);
    REQUIRE(f.good());
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Round-trips back to the same plaintext through Load.
    ConfigManager::InvalidateCache();
    const TrackerConfig out = ConfigManager::Load();
    CHECK(out.PlaneApiKey == secret);

#if defined(_WIN32)
    // On Win32 the secret is DPAPI-encrypted: the plaintext must NOT appear in the file, and the
    // encrypted-at-rest key must be present.
    CHECK(raw.find(secret) == std::string::npos);
    CHECK(raw.find("plane_api_key_enc") != std::string::npos);
#else
    // Non-Win32 has no DPAPI: plaintext fallback is expected (documented behavior).
    CHECK(raw.find(secret) != std::string::npos);
#endif
}

TEST_CASE("ConfigManager Load clamps quick_create_ctx_log_lines to [1, 300]") {
    smatchet_tests::TestEnvGuard env;

    SUBCASE("below the floor") {
        TrackerConfig in;
        in.QuickCreateCtxLogLines = -5;
        ConfigManager::Save(in);
        ConfigManager::InvalidateCache();
        CHECK(ConfigManager::Load().QuickCreateCtxLogLines == 1);
    }

    SUBCASE("above the ceiling") {
        TrackerConfig in;
        in.QuickCreateCtxLogLines = 100000;
        ConfigManager::Save(in);
        ConfigManager::InvalidateCache();
        CHECK(ConfigManager::Load().QuickCreateCtxLogLines == 300);
    }
}
