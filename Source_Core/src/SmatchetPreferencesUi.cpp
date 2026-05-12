#include "SmatchetUI.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "TrackerFieldValueUtils.h"
#include "SmatchetImGuiFonts.h"
#include "FieldCatalogCache.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(SMATCHET_WITH_MCP)
#include "PluginHost.h"
#endif

namespace {

std::string JoinCsv(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += values[i];
    }
    return out;
}

template <size_t N> void CopyStringToBuffer(char (&dst)[N], const std::string& str) {
    static_assert(N > 0, "CopyStringToBuffer requires a non-empty char array");
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(str.size(), cap);
    if (n > 0) {
        std::memcpy(dst, str.data(), n);
    }
}

std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            size_t start = 0;
            size_t end = current.size();
            while (start < end && (current[start] == ' ' || current[start] == '\t'))
                ++start;
            while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t'))
                --end;
            if (end > start) {
                result.push_back(current.substr(start, end - start));
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    size_t start = 0;
    size_t end = current.size();
    while (start < end && (current[start] == ' ' || current[start] == '\t'))
        ++start;
    while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t'))
        --end;
    if (end > start) {
        result.push_back(current.substr(start, end - start));
    }
    return result;
}

} // namespace

void SmatchetUI::drawPreferencesWindow(AppController& app, UiDrawSession& d) {
    static bool s_suggestionsLoaded = false;
    static bool s_templatesLoaded = false;
    static bool s_quickTemplatesLoaded = false;
    static bool s_blameTemplatesLoaded = false;
    if (!d.showPreferences) {
        d.preferencesBuffersLoaded = false;
        d.mcpPrefsSavedHintUntil = {};
        s_suggestionsLoaded = false;
        s_templatesLoaded = false;
        s_quickTemplatesLoaded = false;
        s_blameTemplatesLoaded = false;
        return;
    }

    prepareTopLevelWindow(d, "preferences", 560.0f, 480.0f, d.layoutForceDefaultsFrames > 0);
    if (!ImGui::Begin("Preferences", &d.showPreferences)) {
        ImGui::End();
        return;
    }
    repairTopLevelWindow(d, "preferences", 420.0f, 360.0f);

    if (!d.preferencesBuffersLoaded) {
        CopyStringToBuffer(d.domainBuf, d.cfg.Domain);
        CopyStringToBuffer(d.emailBuf, d.cfg.Email);
        CopyStringToBuffer(d.tokenBuf, d.cfg.ApiToken);
        // PR 6: projectKeyBuf / planeProjectBuf removed — see SmatchetUiSession.h.
        CopyStringToBuffer(d.trackerTypeBuf, d.cfg.TrackerType);
        CopyStringToBuffer(d.planeUrlBuf, d.cfg.PlaneUrl);
        CopyStringToBuffer(d.planeWorkspaceBuf, d.cfg.PlaneWorkspaceSlug);
        CopyStringToBuffer(d.planeApiKeyBuf, d.cfg.PlaneApiKey);
        CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
        CopyStringToBuffer(d.newIssueInheritFieldsPlaneBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsPlane));
#if defined(SMATCHET_WITH_MCP)
        d.mcpEnabled = d.cfg.McpEnabled;
        d.mcpPort = d.cfg.McpPort;
        d.mcpAllowRemote = d.cfg.McpAllowRemote;
        d.mcpAllowLuaExecution = d.cfg.McpAllowLuaExecution;
        CopyStringToBuffer(d.mcpAuthTokenBuf, d.cfg.McpAuthToken);
#endif
        d.preferencesBuffersLoaded = true;
    }

    if (ImGui::BeginTabBar("PreferencesTabs")) {
        if (ImGui::BeginTabItem("Tracker")) {
            ImGui::TextUnformatted("Backend Selection");
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Checkbox("Read-only mode", &d.cfg.ReadOnlyMode)) {
                ConfigManager::Save(d.cfg);
            }
            ImGui::SetItemTooltip("Disables tracker-changing actions such as field edits, issue creation, comments, "
                                  "worklogs, and offline "
                                  "write replay. Enabled by default on first launch before setup.");
            ImGui::Spacing();

            const char* items[] = {"Jira", "Plane"};
            int currentItem = (std::string(d.trackerTypeBuf) == "Plane") ? 1 : 0;
            if (ImGui::Combo("Tracker Backend", &currentItem, items, IM_ARRAYSIZE(items))) {
                CopyStringToBuffer(d.trackerTypeBuf, items[currentItem]);
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (currentItem == 0) {
                ImGui::TextUnformatted("Jira Configuration (Atlassian Cloud)");
                ImGui::InputText("Domain", d.domainBuf, sizeof(d.domainBuf), ImGuiInputTextFlags_CharsNoBlank);
                ImGui::SetItemTooltip("e.g. companyname.atlassian.net");
                ImGui::InputText("Email", d.emailBuf, sizeof(d.emailBuf));
                ImGui::InputText("API Token", d.tokenBuf, sizeof(d.tokenBuf), ImGuiInputTextFlags_Password);
                // PR 6: "Project Key" preference row removed. Project is per-operation — picked
                // via the new-issue draft picker, derived from the active view's JQL, or supplied
                // on ticket.create. The "Recently used projects" section below surfaces cached
                // projects for visibility / Forget.
                ImGui::Spacing();
                ImGui::InputText("New issue: inherit fields from last row (Jira)", d.newIssueInheritFieldsBuf,
                                 sizeof(d.newIssueInheritFieldsBuf));
                ImGui::SetItemTooltip(
                    "Comma-separated Jira field ids copied from the last grid row when you click + New issue "
                    "(e.g. description, priority, assignee, labels, components).");
            } else {
                ImGui::TextUnformatted("Plane Configuration (plane.so)");
                ImGui::InputText("URL", d.planeUrlBuf, sizeof(d.planeUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
                ImGui::SetItemTooltip("e.g. https://api.plane.so");
                ImGui::InputText("Workspace Slug", d.planeWorkspaceBuf, sizeof(d.planeWorkspaceBuf),
                                 ImGuiInputTextFlags_CharsNoBlank);
                // PR 6: "Project ID (UUID)" preference row removed. See Jira note above.
                ImGui::InputText("API Key", d.planeApiKeyBuf, sizeof(d.planeApiKeyBuf), ImGuiInputTextFlags_Password);
                ImGui::Spacing();
                ImGui::InputText("New issue: inherit fields from last row (Plane)", d.newIssueInheritFieldsPlaneBuf,
                                 sizeof(d.newIssueInheritFieldsPlaneBuf));
                ImGui::SetItemTooltip(
                    "Comma-separated Plane field ids copied from the last grid row when you click + New issue "
                    "(e.g. description, priority, assignee, labels).");
            }
            ImGui::Spacing();

            // PR 6: "Recently used projects" — read-only listbox sourced from FieldCatalogCache,
            // filtered to the current backend + endpoint. Replaces the deleted "Project Key" /
            // "Project ID (UUID)" preference rows. Each row has a Forget button.
            ImGui::Separator();
            ImGui::TextUnformatted(SmatchetLocalization::T("prefs.recentProjects", "Recently used projects"));
            {
                const std::string backendKind = (currentItem == 1) ? std::string("Plane") : std::string("Jira");
                const std::string endpoint =
                    (currentItem == 1)
                        ? (std::string(d.planeUrlBuf) + std::string("|") + std::string(d.planeWorkspaceBuf))
                        : std::string(d.domainBuf);
                std::vector<FieldCatalogCache::CachedProjectEntry> cached = FieldCatalogCache::ListCachedProjects();
                // Filter to current backend + endpoint.
                cached.erase(std::remove_if(cached.begin(), cached.end(),
                                            [&](const FieldCatalogCache::CachedProjectEntry& e) {
                                                return e.backend != backendKind || e.endpoint != endpoint;
                                            }),
                             cached.end());
                if (cached.empty()) {
                    ImGui::TextDisabled("%s", SmatchetLocalization::T("prefs.recentProjects.empty", "(none yet)"));
                } else {
                    const char* forgetLabel = SmatchetLocalization::T("prefs.recentProjects.forget", "Forget");
                    for (const auto& entry : cached) {
                        ImGui::PushID(entry.projectKey.c_str());
                        // Format: KEY — lastUsed timestamp. (CachedProjectEntry has no displayName
                        // field today; PR 7 may extend the schema with one — for now the key alone
                        // is enough since the picker UI already resolves display names live.)
                        char timeBuf[32] = {0};
                        const std::time_t t = static_cast<std::time_t>(entry.lastUsedUnix);
                        std::tm tmv{};
#if defined(_WIN32)
                        localtime_s(&tmv, &t);
#else
                        localtime_r(&t, &tmv);
#endif
                        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tmv);
                        ImGui::Text("%s — %s", entry.projectKey.c_str(), timeBuf);
                        ImGui::SameLine();
                        if (ImGui::SmallButton(forgetLabel)) {
                            FieldCatalogCache::ForgetProject(entry.projectKey, entry.backend, entry.endpoint);
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Query/JQL and column fields are configured in the Views dashboard.");
            if (ImGui::Button("Open Views Dashboard")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            ImGui::EndTabItem();
        }
#if defined(SMATCHET_WITH_MCP)
        if (ImGui::BeginTabItem("Integrations")) {
            ImGui::TextUnformatted("MCP (Model Context Protocol)");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Checkbox("Enable MCP server", &d.mcpEnabled);
            ImGui::InputInt("MCP Port", &d.mcpPort);
            if (d.mcpPort < 1) {
                d.mcpPort = 1;
            }
            if (d.mcpPort > 65535) {
                d.mcpPort = 65535;
            }
            ImGui::Checkbox("Bind on all interfaces (LAN)", &d.mcpAllowRemote);
            ImGui::SetItemTooltip(
                "When off, MCP listens on localhost only (127.0.0.1). When on, it binds 0.0.0.0 — reachable on your "
                "network. Set an auth token below if you enable this.");
            ImGui::InputText("MCP auth token (optional)", d.mcpAuthTokenBuf, sizeof(d.mcpAuthTokenBuf),
                             ImGuiInputTextFlags_Password);
            ImGui::SetItemTooltip("If set, clients must send header X-Smatchet-Token with this value. If empty and "
                                  "bind is localhost-only, "
                                  "only loopback clients may connect.");
            ImGui::Checkbox("Allow MCP run_lua tool (dangerous)", &d.mcpAllowLuaExecution);
            ImGui::SetItemTooltip("Off by default. When enabled, MCP clients can execute Lua snippets or Scripts/*.lua "
                                  "via the built-in run_lua tool.");
            {
                const std::string tokenBufStr(d.mcpAuthTokenBuf);
                const bool dirty = (d.mcpEnabled != d.cfg.McpEnabled) || (d.mcpPort != d.cfg.McpPort) ||
                                   (d.mcpAllowRemote != d.cfg.McpAllowRemote) ||
                                   (d.mcpAllowLuaExecution != d.cfg.McpAllowLuaExecution) ||
                                   (tokenBufStr != d.cfg.McpAuthToken);
                if (dirty) {
                    d.cfg.McpEnabled = d.mcpEnabled;
                    d.cfg.McpPort = d.mcpPort;
                    d.cfg.McpAllowRemote = d.mcpAllowRemote;
                    d.cfg.McpAllowLuaExecution = d.mcpAllowLuaExecution;
                    d.cfg.McpAuthToken = tokenBufStr;
                    ConfigManager::Save(d.cfg);
                    LOG_INFO("Preferences: MCP settings saved (McpEnabled=%d port=%d)",
                             static_cast<int>(d.cfg.McpEnabled), d.cfg.McpPort);
                    app.AppendMcpActivity("MCP: Integrations saved settings to disk; syncing plugin host.");
                    ::PluginHost* ph = app.RuntimePluginHost();
                    if (ph != nullptr) {
                        ph->SyncMcpPluginWithConfig(app, d.cfg);
                    } else {
                        app.AppendMcpActivity("MCP: no runtime plugin host — restart app to load MCP plugin.");
                    }
                    d.mcpPrefsSavedHintUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
                }
            }
            if (std::chrono::steady_clock::now() < d.mcpPrefsSavedHintUntil) {
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "MCP settings saved to disk.");
                if (app.RuntimePluginHost() != nullptr) {
                    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f),
                                       "MCP server start/stop applied for this session (standalone / embedded host).");
                }
            }
            ImGui::TextDisabled(
                "MCP settings save when changed. With a running app host, the MCP server starts or stops immediately; "
                "otherwise restart the app once.");
            ImGui::Spacing();
            ImGui::TextDisabled(
                "Runtime status, endpoints, and action log: Automation -> Agent Bridge (MCP)... (separate window).");
            ImGui::EndTabItem();
        }
#endif
        if (ImGui::BeginTabItem("Local data")) {
            ImGui::TextWrapped(
                "Stored tickets, offline create queues, and pending field edits live in a local SQLite file. "
                "Recreating it clears that data only; tracker credentials and views are not removed. A full "
                "issue refresh runs afterward.");
            ImGui::Spacing();
            const std::string resolved = app.GetResolvedLocalCacheDbPath();
            if (!resolved.empty()) {
                ImGui::TextDisabled("Cache file:");
                ImGui::SameLine();
                ::ImGui::TextWrapped("%s", resolved.c_str());
            }
            ImGui::Spacing();
            if (ImGui::Button("Recreate database...")) {
                ImGui::OpenPopup("Delete local database?###RecreateSqliteDbConfirm");
            }
            ImGui::SetItemTooltip("Permanently delete the local cache file and start with an empty database.");

            if (ImGui::BeginPopupModal("Delete local database?###RecreateSqliteDbConfirm", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextWrapped(
                    "This removes cached issues and any queued offline writes stored on this machine. It does not "
                    "delete anything on the tracker. Continue?");
                ImGui::Separator();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete and recreate")) {
                    std::string err;
                    if (app.RecreateLocalCacheDatabase(err)) {
                        SmatchetToastManager::Instance().Push(
                            std::string(SmatchetLocalization::T("toast.success", "Success")),
                            std::string(SmatchetLocalization::T("toast.local_db_recreated",
                                                                "Local database recreated; refreshing issues.")),
                            ToastType::Success, 4000);
                        app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
                        ImGui::CloseCurrentPopup();
                    } else {
                        const char* detail = SmatchetLocalization::T("toast.local_db_recreate_failed_detail",
                                                                     "Could not recreate the local database.");
                        SmatchetToastManager::Instance().Push(
                            std::string(SmatchetLocalization::T("toast.local_db_error_title", "Local database")),
                            err.empty() ? std::string(detail) : err, ToastType::Error, 6000);
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            ImGui::TextUnformatted("Application Typography");
            ImGui::Separator();
            ImGui::Spacing();

            const char* fonts[] = {
                "Segoe UI",       "Proggy (Clean/Default)", "Consolas",     "Arial",  "Courier New", "Georgia",
                "Lucida Console", "Microsoft Sans Serif",   "Trebuchet MS", "Verdana"};
            int currentFontIdx = 0;
            for (int i = 0; i < 10; ++i) {
                if (d.cfg.SelectedFontName == fonts[i]) {
                    currentFontIdx = i;
                    break;
                }
            }
            if (ImGui::Combo("Application Font", &currentFontIdx, fonts, 10)) {
                d.cfg.SelectedFontName = fonts[currentFontIdx];
                ConfigManager::Save(d.cfg);
                SmatchetRequestFontReload(d.cfg.SelectedFontName, 16.0f);
            }
            ImGui::SetItemTooltip(
                "Select the typography for the entire application. Rebuilds and reloads the font atlas instantly.");

            const auto& languages = SmatchetLocalization::AvailableLanguages();
            int currentLanguageIdx = 0;
            for (int i = 0; i < static_cast<int>(languages.size()); ++i) {
                if (d.cfg.UiLanguage == languages[static_cast<size_t>(i)].Code) {
                    currentLanguageIdx = i;
                    break;
                }
            }
            const char* languageItems[] = {SmatchetLocalization::T("language.en_us", "English"),
                                           SmatchetLocalization::T("language.fr_native", u8"Français")};
            if (ImGui::Combo("Language", &currentLanguageIdx, languageItems, IM_ARRAYSIZE(languageItems))) {
                if (currentLanguageIdx >= 0 && currentLanguageIdx < static_cast<int>(languages.size())) {
                    d.cfg.UiLanguage = languages[static_cast<size_t>(currentLanguageIdx)].Code;
                    ConfigManager::Save(d.cfg);
                    SmatchetLocalization::SetLanguage(d.cfg.UiLanguage);
                }
            }
            ImGui::SetItemTooltip(
                "Select the UI language. App-owned UI text changes immediately; tracker data is shown as-is.");

            ImGui::Spacing();
            ImGui::TextUnformatted("Grid and field text");
            ImGui::Separator();
            if (ImGui::Checkbox("Show tooltips when text overflows", &d.cfg.EnableFieldOverflowTooltips)) {
                ConfigManager::Save(d.cfg);
            }
            ImGui::SetItemTooltip(
                "When a value is truncated to fit the cell, or spans multiple lines, hover to read the full text in a "
                "tooltip.");
            int gridWheelSwallowTicks = d.cfg.GridEndWheelSwallowsBeforeHorizontal;
            if (ImGui::InputInt("Wheel ticks before horizontal scroll", &gridWheelSwallowTicks)) {
                if (gridWheelSwallowTicks < 0) {
                    gridWheelSwallowTicks = 0;
                }
                if (gridWheelSwallowTicks > 32) {
                    gridWheelSwallowTicks = 32;
                }
                d.cfg.GridEndWheelSwallowsBeforeHorizontal = gridWheelSwallowTicks;
                ConfigManager::Save(d.cfg);
            }
            ImGui::SetItemTooltip(
                "At top/bottom of the ticket grid, vertical wheel starts horizontal scrolling after this many wheel "
                "ticks. 0 routes immediately.");

            ImGui::Spacing();
            ImGui::TextUnformatted("Date Formatting");
            ImGui::Separator();
            ImGui::Spacing();

            const char* dateFormats[] = {"Relative / Compact", "Always Relative", "Absolute ISO", "Absolute Friendly"};
            int currentDateFormatIdx = 0;
            if (d.cfg.DateFormatOption == "always_relative") {
                currentDateFormatIdx = 1;
            } else if (d.cfg.DateFormatOption == "absolute_iso") {
                currentDateFormatIdx = 2;
            } else if (d.cfg.DateFormatOption == "absolute_friendly") {
                currentDateFormatIdx = 3;
            }

            if (ImGui::Combo("Date Format Style", &currentDateFormatIdx, dateFormats, IM_ARRAYSIZE(dateFormats))) {
                if (currentDateFormatIdx == 0) {
                    d.cfg.DateFormatOption = "compact";
                } else if (currentDateFormatIdx == 1) {
                    d.cfg.DateFormatOption = "always_relative";
                } else if (currentDateFormatIdx == 2) {
                    d.cfg.DateFormatOption = "absolute_iso";
                } else if (currentDateFormatIdx == 3) {
                    d.cfg.DateFormatOption = "absolute_friendly";
                }
                ConfigManager::Save(d.cfg);
            }
            ImGui::SetItemTooltip("Select how date and datetime values are rendered in the grids and UI panels.");

            if (currentDateFormatIdx == 0) {
                int threshold = d.cfg.DateCompactRelativeThresholdDays;
                if (ImGui::SliderInt("Compact Relative Threshold (Days)", &threshold, 1, 90, "%d days")) {
                    d.cfg.DateCompactRelativeThresholdDays = threshold;
                    ConfigManager::Save(d.cfg);
                }
                ImGui::SetItemTooltip("Threshold in days where the compact view transitions from relative (e.g. -3d) "
                                      "to short absolute (e.g. May 07 '26).");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Updates");
            ImGui::Separator();
            if (ImGui::Checkbox("Check for updates automatically", &d.cfg.UpdateCheckEnabled)) {
                ConfigManager::Save(d.cfg);
            }
            if (ImGui::Checkbox("Include prerelease builds", &d.cfg.UpdateIncludePrerelease)) {
                ConfigManager::Save(d.cfg);
            }
            ImGui::SetItemTooltip("When enabled, startup and manual checks can target prerelease GitHub releases too.");
            if (ImGui::Button("Check for Updates Now")) {
                d.appUpdateStartupCheckStarted = true;
                d.appUpdateActionStatus.clear();
                d.appUpdateCheckManual = true;
                d.appUpdateCheckInFlight = true;
                d.appUpdateFuture = std::async(std::launch::async, [&app, cfg = d.cfg]() {
                    return app.CheckForAppUpdate(cfg.UpdateIncludePrerelease);
                });
            }
            if (d.appUpdateCheckInFlight) {
                ImGui::SameLine();
                ImGui::TextDisabled("(checking...)");
            }
            if (!d.cfg.UpdateSkipVersion.empty()) {
                ImGui::TextDisabled("Skipped version: %s", d.cfg.UpdateSkipVersion.c_str());
                if (ImGui::SmallButton("Clear skipped version")) {
                    d.cfg.UpdateSkipVersion.clear();
                    ConfigManager::Save(d.cfg);
                }
            }
            ImGui::TextDisabled("GitHub release repo: %s", d.cfg.UpdateGithubRepo.c_str());

            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Fields Inputs")) {
            if (ImGui::BeginTabBar("FieldsInputsSubTabBar")) {
                if (ImGui::BeginTabItem("Time Estimates")) {
                    ImGui::TextUnformatted("Duration Suggestions");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize the default options displayed in the dropdown menus for Original "
                                        "Estimate, Remaining Estimate, and Time Spent fields.");
                    ImGui::Spacing();

                    static std::vector<std::string> s_suggestionsList;
                    if (!s_suggestionsLoaded) {
                        s_suggestionsList = TrackerFieldValueUtils::LoadDurationSuggestions();
                        s_suggestionsLoaded = true;
                    }

                    // Render list of current suggestions in a premium boxed child frame
                    ImGui::Text("Current Suggestions:");
                    ImGui::BeginChild("SuggestionsListChild", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_suggestionsList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));

                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(s_suggestionsList[i].c_str());

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_suggestionsList[i], s_suggestionsList[i - 1]);
                                TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_suggestionsList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_suggestionsList[i], s_suggestionsList[i + 1]);
                                TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_suggestionsList.erase(s_suggestionsList.begin() + i);
                            TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            --i;
                        }
                        ImGui::PopStyleColor();

                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Inline add controls
                    static char s_prefNewSuggestionBuf[16] = "";
                    ImGui::Text("Add Custom Suggestion");
                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::InputText("##PrefNewSuggestion", s_prefNewSuggestionBuf, sizeof(s_prefNewSuggestionBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Add Option", ImVec2(90.0f, 0.0f)) ||
                        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        std::string newVal = s_prefNewSuggestionBuf;
                        if (!newVal.empty()) {
                            if (std::find(s_suggestionsList.begin(), s_suggestionsList.end(), newVal) ==
                                s_suggestionsList.end()) {
                                s_suggestionsList.push_back(newVal);
                                TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            }
                            s_prefNewSuggestionBuf[0] = '\0';
                        }
                    }
                    ImGui::SetItemTooltip("Enter duration strings e.g. '15m', '2h', '3.5h', '1d', '2w'");

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Work Log Templates")) {
                    ImGui::TextUnformatted("Work Log Description Templates");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize the quick comment templates displayed in the 'Templates' dropdown "
                                        "next to the Log Work description field.");
                    ImGui::Spacing();

                    static std::vector<std::string> s_templatesList;
                    if (!s_templatesLoaded) {
                        s_templatesList = TrackerFieldValueUtils::LoadCommentTemplates();
                        s_templatesLoaded = true;
                    }

                    // Render list of current comment templates in a premium boxed child frame
                    ImGui::Text("Current Comment Templates:");
                    ImGui::BeginChild("TemplatesListChild", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_templatesList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));

                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(s_templatesList[i].c_str());

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_templatesList[i], s_templatesList[i - 1]);
                                TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_templatesList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_templatesList[i], s_templatesList[i + 1]);
                                TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_templatesList.erase(s_templatesList.begin() + i);
                            TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            --i;
                        }
                        ImGui::PopStyleColor();

                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Inline add controls for templates
                    static char s_prefNewTemplateBuf[128] = "";
                    ImGui::Text("Add Comment Template");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f);
                    ImGui::InputText("##PrefNewTemplate", s_prefNewTemplateBuf, sizeof(s_prefNewTemplateBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Add Template", ImVec2(100.0f, 0.0f)) ||
                        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        std::string newVal = s_prefNewTemplateBuf;
                        if (!newVal.empty()) {
                            if (std::find(s_templatesList.begin(), s_templatesList.end(), newVal) ==
                                s_templatesList.end()) {
                                s_templatesList.push_back(newVal);
                                TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            }
                            s_prefNewTemplateBuf[0] = '\0';
                        }
                    }
                    ImGui::SetItemTooltip("Enter template text, e.g. 'Investigated and resolved issue #123.'");

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Quick Comments")) {
                    ImGui::TextUnformatted("Grid Right-Click Quick Comments");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize templates displayed when right-clicking issue cells in the grid. "
                                        "Placeholders: {key} (or {issueKey})");
                    ImGui::Spacing();

                    static std::vector<CommentTemplate> s_quickTemplatesList;
                    if (!s_quickTemplatesLoaded) {
                        s_quickTemplatesList = d.cfg.QuickCommentTemplates;
                        s_quickTemplatesLoaded = true;
                    }
                    static int s_selectedQuickIdx = -1;
                    if (s_selectedQuickIdx >= static_cast<int>(s_quickTemplatesList.size())) {
                        s_selectedQuickIdx = static_cast<int>(s_quickTemplatesList.size()) - 1;
                    }

                    // Render list of current comment templates on top
                    ImGui::BeginChild("QuickListChild", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_quickTemplatesList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::AlignTextToFramePadding();
                        std::string displayName =
                            s_quickTemplatesList[i].Title + " (" + s_quickTemplatesList[i].Id + ")";
                        if (ImGui::Selectable(displayName.c_str(), s_selectedQuickIdx == static_cast<int>(i))) {
                            s_selectedQuickIdx = static_cast<int>(i);
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_quickTemplatesList[i], s_quickTemplatesList[i - 1]);
                                if (s_selectedQuickIdx == static_cast<int>(i))
                                    s_selectedQuickIdx = static_cast<int>(i - 1);
                                else if (s_selectedQuickIdx == static_cast<int>(i - 1))
                                    s_selectedQuickIdx = static_cast<int>(i);
                                d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                                ConfigManager::Save(d.cfg);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_quickTemplatesList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_quickTemplatesList[i], s_quickTemplatesList[i + 1]);
                                if (s_selectedQuickIdx == static_cast<int>(i))
                                    s_selectedQuickIdx = static_cast<int>(i + 1);
                                else if (s_selectedQuickIdx == static_cast<int>(i + 1))
                                    s_selectedQuickIdx = static_cast<int>(i);
                                d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                                ConfigManager::Save(d.cfg);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_quickTemplatesList.erase(s_quickTemplatesList.begin() + i);
                            if (s_selectedQuickIdx == static_cast<int>(i)) {
                                s_selectedQuickIdx = -1;
                            } else if (s_selectedQuickIdx > static_cast<int>(i)) {
                                s_selectedQuickIdx--;
                            }
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            ConfigManager::Save(d.cfg);
                            --i;
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();

                    // Detail section / Add section
                    if (s_selectedQuickIdx >= 0 && s_selectedQuickIdx < static_cast<int>(s_quickTemplatesList.size())) {
                        auto& t = s_quickTemplatesList[s_selectedQuickIdx];
                        ImGui::TextDisabled("Edit Selected Template details:");

                        static char titleBuf[64] = "";
                        static char idBuf[64] = "";
                        static char textBuf[512] = "";

                        // Copy to buffer if different to avoid typing overwrites
                        static int lastSelectedIdx = -2;
                        if (lastSelectedIdx != s_selectedQuickIdx) {
                            std::strncpy(titleBuf, t.Title.c_str(), sizeof(titleBuf) - 1);
                            titleBuf[sizeof(titleBuf) - 1] = '\0';
                            std::strncpy(idBuf, t.Id.c_str(), sizeof(idBuf) - 1);
                            idBuf[sizeof(idBuf) - 1] = '\0';
                            std::strncpy(textBuf, t.Text.c_str(), sizeof(textBuf) - 1);
                            textBuf[sizeof(textBuf) - 1] = '\0';
                            lastSelectedIdx = s_selectedQuickIdx;
                        }

                        ImGui::TextUnformatted("Title:");
                        ImGui::SameLine(60.0f);
                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::InputText("##EditQuickTitle", titleBuf, sizeof(titleBuf))) {
                            t.Title = titleBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            ConfigManager::Save(d.cfg);
                        }

                        ImGui::SameLine(280.0f);
                        ImGui::TextUnformatted("ID:");
                        ImGui::SameLine(310.0f);
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::InputText("##EditQuickId", idBuf, sizeof(idBuf))) {
                            t.Id = idBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            ConfigManager::Save(d.cfg);
                        }

                        ImGui::TextUnformatted("Body:");
                        if (ImGui::InputTextMultiline("##EditQuickText", textBuf, sizeof(textBuf),
                                                      ImVec2(-FLT_MIN, 60.0f))) {
                            t.Text = textBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            ConfigManager::Save(d.cfg);
                        }
                    } else {
                        ImGui::TextDisabled("Select a template above to view or edit its details.");
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("+ Add New Template", ImVec2(160.0f, 0.0f))) {
                        CommentTemplate t;
                        t.Title = "New Template";
                        t.Id = "new_template";
                        t.Text = "Template text for {key}";
                        s_quickTemplatesList.push_back(t);
                        s_selectedQuickIdx = static_cast<int>(s_quickTemplatesList.size()) - 1;
                        d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                        ConfigManager::Save(d.cfg);
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Blame Comments")) {
                    ImGui::TextUnformatted("Blame Analysis Quick Comments");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize templates displayed when clicking on the Blame Analysis rows. "
                                        "Placeholders: {key}, {path}, {line}, {cl}, {user}, {function}");
                    ImGui::Spacing();

                    static std::vector<CommentTemplate> s_blameTemplatesList;
                    if (!s_blameTemplatesLoaded) {
                        s_blameTemplatesList = d.cfg.BlameCommentTemplates;
                        s_blameTemplatesLoaded = true;
                    }
                    static int s_selectedBlameIdx = -1;
                    if (s_selectedBlameIdx >= static_cast<int>(s_blameTemplatesList.size())) {
                        s_selectedBlameIdx = static_cast<int>(s_blameTemplatesList.size()) - 1;
                    }

                    // Render list of current comment templates on top
                    ImGui::BeginChild("BlameListChild", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_blameTemplatesList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::AlignTextToFramePadding();
                        std::string displayName =
                            s_blameTemplatesList[i].Title + " (" + s_blameTemplatesList[i].Id + ")";
                        if (ImGui::Selectable(displayName.c_str(), s_selectedBlameIdx == static_cast<int>(i))) {
                            s_selectedBlameIdx = static_cast<int>(i);
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_blameTemplatesList[i], s_blameTemplatesList[i - 1]);
                                if (s_selectedBlameIdx == static_cast<int>(i))
                                    s_selectedBlameIdx = static_cast<int>(i - 1);
                                else if (s_selectedBlameIdx == static_cast<int>(i - 1))
                                    s_selectedBlameIdx = static_cast<int>(i);
                                d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                                ConfigManager::Save(d.cfg);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_blameTemplatesList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_blameTemplatesList[i], s_blameTemplatesList[i + 1]);
                                if (s_selectedBlameIdx == static_cast<int>(i))
                                    s_selectedBlameIdx = static_cast<int>(i + 1);
                                else if (s_selectedBlameIdx == static_cast<int>(i + 1))
                                    s_selectedBlameIdx = static_cast<int>(i);
                                d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                                ConfigManager::Save(d.cfg);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_blameTemplatesList.erase(s_blameTemplatesList.begin() + i);
                            if (s_selectedBlameIdx == static_cast<int>(i)) {
                                s_selectedBlameIdx = -1;
                            } else if (s_selectedBlameIdx > static_cast<int>(i)) {
                                s_selectedBlameIdx--;
                            }
                            d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                            ConfigManager::Save(d.cfg);
                            --i;
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();

                    // Detail section / Add section
                    if (s_selectedBlameIdx >= 0 && s_selectedBlameIdx < static_cast<int>(s_blameTemplatesList.size())) {
                        auto& t = s_blameTemplatesList[s_selectedBlameIdx];
                        ImGui::TextDisabled("Edit Selected Template details:");

                        static char titleBuf[64] = "";
                        static char idBuf[64] = "";
                        static char textBuf[512] = "";

                        // Copy to buffer if different to avoid typing overwrites
                        static int lastSelectedIdx = -2;
                        if (lastSelectedIdx != s_selectedBlameIdx) {
                            std::strncpy(titleBuf, t.Title.c_str(), sizeof(titleBuf) - 1);
                            titleBuf[sizeof(titleBuf) - 1] = '\0';
                            std::strncpy(idBuf, t.Id.c_str(), sizeof(idBuf) - 1);
                            idBuf[sizeof(idBuf) - 1] = '\0';
                            std::strncpy(textBuf, t.Text.c_str(), sizeof(textBuf) - 1);
                            textBuf[sizeof(textBuf) - 1] = '\0';
                            lastSelectedIdx = s_selectedBlameIdx;
                        }

                        ImGui::TextUnformatted("Title:");
                        ImGui::SameLine(60.0f);
                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::InputText("##EditBlameTitle", titleBuf, sizeof(titleBuf))) {
                            t.Title = titleBuf;
                            d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                            ConfigManager::Save(d.cfg);
                        }

                        ImGui::SameLine(280.0f);
                        ImGui::TextUnformatted("ID:");
                        ImGui::SameLine(310.0f);
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::InputText("##EditBlameId", idBuf, sizeof(idBuf))) {
                            t.Id = idBuf;
                            d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                            ConfigManager::Save(d.cfg);
                        }

                        ImGui::TextUnformatted("Body:");
                        if (ImGui::InputTextMultiline("##EditBlameText", textBuf, sizeof(textBuf),
                                                      ImVec2(-FLT_MIN, 60.0f))) {
                            t.Text = textBuf;
                            d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                            ConfigManager::Save(d.cfg);
                        }
                    } else {
                        ImGui::TextDisabled("Select a template above to view or edit its details.");
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("+ Add New Template", ImVec2(160.0f, 0.0f))) {
                        CommentTemplate t;
                        t.Title = "New Template";
                        t.Id = "new_template";
                        t.Text = "Template text for {key}";
                        s_blameTemplatesList.push_back(t);
                        s_selectedBlameIdx = static_cast<int>(s_blameTemplatesList.size()) - 1;
                        d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                        ConfigManager::Save(d.cfg);
                    }

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Blame Analysis")) {
            blameAnalysisUi_.DrawBlamePreferencesTab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "Save & Sync writes the Tracker tab (and optional Integrations tab when enabled in this build) to "
        "disk and refreshes the tracker connection. MCP runtime status: Automation -> Agent Bridge (MCP).... "
        "Appearance options save immediately when changed. Log level and verbose logging: Inspect -> Runtime Log. The "
        "Blame "
        "Analysis tab has its own Save "
        "settings and Reload settings buttons.");
    ImGui::Spacing();
    if (ImGui::Button("Save & Sync", ImVec2(140.0f, 0.0f))) {
        d.cfg.Domain = d.domainBuf;
        d.cfg.Email = d.emailBuf;
        d.cfg.ApiToken = d.tokenBuf;
        // PR 6: ProjectKey / PlaneProjectId writebacks removed — project is per-operation.
        d.cfg.TrackerType = d.trackerTypeBuf;
        d.cfg.PlaneUrl = d.planeUrlBuf;
        d.cfg.PlaneWorkspaceSlug = d.planeWorkspaceBuf;
        d.cfg.PlaneApiKey = d.planeApiKeyBuf;
        {
            std::vector<std::string> parsedInherit = ParseCsv(std::string(d.newIssueInheritFieldsBuf));
            d.cfg.NewIssueInheritFieldIds.clear();
            for (const auto& s : parsedInherit) {
                if (!s.empty() && s != "summary") {
                    d.cfg.NewIssueInheritFieldIds.push_back(s);
                }
            }
            if (d.cfg.NewIssueInheritFieldIds.empty()) {
                d.cfg.NewIssueInheritFieldIds = IssueDraftHelpers::DefaultNewIssueInheritFieldIds();
            }
            CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
        }
        {
            std::vector<std::string> parsedInherit = ParseCsv(std::string(d.newIssueInheritFieldsPlaneBuf));
            d.cfg.NewIssueInheritFieldIdsPlane.clear();
            for (const auto& s : parsedInherit) {
                if (!s.empty() && s != "summary") {
                    d.cfg.NewIssueInheritFieldIdsPlane.push_back(s);
                }
            }
            if (d.cfg.NewIssueInheritFieldIdsPlane.empty()) {
                d.cfg.NewIssueInheritFieldIdsPlane = IssueDraftHelpers::DefaultNewIssueInheritFieldIds();
            }
            CopyStringToBuffer(d.newIssueInheritFieldsPlaneBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsPlane));
        }
#if defined(SMATCHET_WITH_MCP)
        d.cfg.McpEnabled = d.mcpEnabled;
        d.cfg.McpPort = d.mcpPort;
        d.cfg.McpAllowRemote = d.mcpAllowRemote;
        d.cfg.McpAllowLuaExecution = d.mcpAllowLuaExecution;
        d.cfg.McpAuthToken = d.mcpAuthTokenBuf;
#endif

        ConfigManager::Save(d.cfg);
#if defined(SMATCHET_WITH_MCP)
        app.AppendMcpActivity("MCP: Save & Sync wrote MCP fields; syncing plugin host.");
        if (::PluginHost* ph = app.RuntimePluginHost()) {
            ph->SyncMcpPluginWithConfig(app, d.cfg);
        } else {
            app.AppendMcpActivity("MCP: no runtime plugin host — restart app to load MCP plugin.");
        }
#endif
        if (d.cfg.TrackerType == "Plane") {
            LOG_INFO("Updated tracker config (Plane). URL='%s', Workspace='%s' (project is per-operation)",
                     d.cfg.PlaneUrl.c_str(), d.cfg.PlaneWorkspaceSlug.c_str());
        } else {
            LOG_INFO("Updated tracker config (Jira). Domain='%s', Email='%s'", d.cfg.Domain.c_str(),
                     d.cfg.Email.c_str());
        }
        d.triggerCatalogRefetch = true;
        const std::string oldBackend = d.lastViewsBackendKey;
        ViewState.EnsureLoaded(d.cfg);
        const std::string newBackend = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
        if (oldBackend != newBackend) {
            d.lastViewsBackendKey = newBackend;
            const ViewDefinition* activeView = ViewState.GetActiveView();
            if (activeView) {
                d.cfg.JqlQuery = activeView->Jql;
                d.cfg.SelectedFields = activeView->Fields;
            }
        }
        app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
    }

    ImGui::End();
}
