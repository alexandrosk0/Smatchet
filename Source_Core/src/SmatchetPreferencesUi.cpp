#include "SmatchetUI.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "Logger.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

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
    if (!d.showPreferences) {
        d.preferencesBuffersLoaded = false;
        d.mcpPrefsSavedHintUntil = {};
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &d.showPreferences)) {
        ImGui::End();
        return;
    }

    if (!d.preferencesBuffersLoaded) {
        CopyStringToBuffer(d.domainBuf, d.cfg.Domain);
        CopyStringToBuffer(d.emailBuf, d.cfg.Email);
        CopyStringToBuffer(d.tokenBuf, d.cfg.ApiToken);
        CopyStringToBuffer(d.projectKeyBuf, d.cfg.ProjectKey);
        CopyStringToBuffer(d.trackerTypeBuf, d.cfg.TrackerType);
        CopyStringToBuffer(d.planeUrlBuf, d.cfg.PlaneUrl);
        CopyStringToBuffer(d.planeWorkspaceBuf, d.cfg.PlaneWorkspaceSlug);
        CopyStringToBuffer(d.planeProjectBuf, d.cfg.PlaneProjectId);
        CopyStringToBuffer(d.planeApiKeyBuf, d.cfg.PlaneApiKey);
        CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
        CopyStringToBuffer(d.newIssueInheritFieldsPlaneBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsPlane));
#if defined(SMATCHET_WITH_AI)
        CopyStringToBuffer(d.aiApiKeyBuf, d.cfg.AiApiKey);
        CopyStringToBuffer(d.aiModelBuf, d.cfg.AiModel);
        CopyStringToBuffer(d.aiBaseUrlBuf, d.cfg.AiBaseUrl);
#endif
#if defined(SMATCHET_WITH_MCP)
        d.mcpEnabled = d.cfg.McpEnabled;
        d.mcpPort = d.cfg.McpPort;
        d.mcpAllowRemote = d.cfg.McpAllowRemote;
        CopyStringToBuffer(d.mcpAuthTokenBuf, d.cfg.McpAuthToken);
#endif
        d.preferencesBuffersLoaded = true;
    }

    if (ImGui::BeginTabBar("PreferencesTabs")) {
        if (ImGui::BeginTabItem("Tracker")) {
            ImGui::TextUnformatted("Backend Selection");
            ImGui::Separator();
            ImGui::Spacing();

            const char* items[] = { "Jira", "Plane" };
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
                ImGui::InputText("Project Key", d.projectKeyBuf, sizeof(d.projectKeyBuf),
                                 ImGuiInputTextFlags_CharsUppercase);
                ImGui::SetItemTooltip("Used for create meta enrichment, e.g. PROJ");
            } else {
                ImGui::TextUnformatted("Plane Configuration (plane.so)");
                ImGui::InputText("URL", d.planeUrlBuf, sizeof(d.planeUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
                ImGui::SetItemTooltip("e.g. https://api.plane.so");
                ImGui::InputText("Workspace Slug", d.planeWorkspaceBuf, sizeof(d.planeWorkspaceBuf), ImGuiInputTextFlags_CharsNoBlank);
                ImGui::InputText("Project ID (UUID)", d.planeProjectBuf, sizeof(d.planeProjectBuf), ImGuiInputTextFlags_CharsNoBlank);
                ImGui::InputText("API Key", d.planeApiKeyBuf, sizeof(d.planeApiKeyBuf), ImGuiInputTextFlags_Password);
            }

            ImGui::Spacing();
            char* inheritBuf = (currentItem == 1) ? d.newIssueInheritFieldsPlaneBuf : d.newIssueInheritFieldsBuf;
            size_t inheritBufSize = (currentItem == 1) ? sizeof(d.newIssueInheritFieldsPlaneBuf) : sizeof(d.newIssueInheritFieldsBuf);
            ImGui::InputText("New issue: inherit fields from last row", inheritBuf, inheritBufSize);
            ImGui::SetItemTooltip(
                "Comma-separated tracker field ids copied from the last grid row when you click + New issue "
                "(e.g. description, priority, assignee, labels, components). "
                "Summary is never copied from the last row. "
                "Clear and Save & Sync to restore the built-in default list.");
            ImGui::Spacing();
            ImGui::TextWrapped("Query/JQL and column fields are configured in the Views dashboard.");
            if (ImGui::Button("Open Views Dashboard")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            ImGui::EndTabItem();
        }
#if defined(SMATCHET_WITH_AI)
        if (ImGui::BeginTabItem("Assistant")) {
            ImGui::TextUnformatted("OpenAI-compatible API used by the AI assistant panel.");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::InputText("API Key", d.aiApiKeyBuf, sizeof(d.aiApiKeyBuf), ImGuiInputTextFlags_Password);
            ImGui::InputText("Model", d.aiModelBuf, sizeof(d.aiModelBuf));
            ImGui::SetItemTooltip("Example: gpt-4o-mini");
            ImGui::InputText("Base URL", d.aiBaseUrlBuf, sizeof(d.aiBaseUrlBuf));
            ImGui::SetItemTooltip("Example: https://api.openai.com");
            ImGui::EndTabItem();
        }
#endif
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
            {
                const std::string tokenBufStr(d.mcpAuthTokenBuf);
                const bool dirty = (d.mcpEnabled != d.cfg.McpEnabled) || (d.mcpPort != d.cfg.McpPort) ||
                                     (d.mcpAllowRemote != d.cfg.McpAllowRemote) || (tokenBufStr != d.cfg.McpAuthToken);
                if (dirty) {
                    d.cfg.McpEnabled = d.mcpEnabled;
                    d.cfg.McpPort = d.mcpPort;
                    d.cfg.McpAllowRemote = d.mcpAllowRemote;
                    d.cfg.McpAuthToken = tokenBufStr;
                    ConfigManager::Save(d.cfg);
                    LOG_INFO("Preferences: MCP settings saved (McpEnabled=%d port=%d)", static_cast<int>(d.cfg.McpEnabled),
                             d.cfg.McpPort);
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
            ImGui::TextDisabled("Runtime status, endpoints, and action log: Scripts → MCP Server… (separate window).");
            ImGui::EndTabItem();
        }
#endif
        if (ImGui::BeginTabItem("Appearance")) {
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
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
            static const LogLevel kLogLevels[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn,
                                                  LogLevel::Error};
            LogLevel parsedLevel = Logger::ParseLogLevelString(d.cfg.LogMinLevel, LogLevel::Info);
            int levelComboIndex = 2;
            for (int i = 0; i < 5; ++i) {
                if (kLogLevels[i] == parsedLevel) {
                    levelComboIndex = i;
                    break;
                }
            }
            ImGui::TextUnformatted("Logging");
            ImGui::Separator();
            ImGui::TextUnformatted("Min log level");
            ImGui::SameLine();
            if (ImGui::Combo("##LogMinLevel", &levelComboIndex,
                             "Trace\0"
                             "Debug\0"
                             "Info\0"
                             "Warn\0"
                             "Error\0"
                             "\0")) {
                d.cfg.LogMinLevel = Logger::LogLevelToString(kLogLevels[levelComboIndex]);
                Logger::Instance().SetMinLevel(kLogLevels[levelComboIndex]);
                ConfigManager::Save(d.cfg);
            }
            bool trackerBodies = d.cfg.LogTrackerHttpBodies;
            if (ImGui::Checkbox("Log Tracker HTTP bodies (truncated)", &trackerBodies)) {
                d.cfg.LogTrackerHttpBodies = trackerBodies;
                Logger::Instance().SetLogTrackerHttpBodies(trackerBodies);
                ConfigManager::Save(d.cfg);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Verbose: logs response text (capped per request). May include issue summaries and user-visible "
                    "data.");
            }
            bool logP4Io = d.cfg.LogP4Io;
            if (ImGui::Checkbox("Log Perforce p4 stdout (truncated, Trace level)", &logP4Io)) {
                d.cfg.LogP4Io = logP4Io;
                Logger::Instance().SetLogP4Io(logP4Io);
                ConfigManager::Save(d.cfg);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Requires min level Trace. Logs capped p4 stdout per command; stderr is logged on non-zero exit.");
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
        "Save & Sync writes the Tracker tab (and optional Assistant / Integrations tabs when enabled in this build) to "
        "disk and refreshes the tracker connection. MCP runtime status: Scripts → MCP Server…. "
        "Appearance and Diagnostics options save immediately when changed. The Blame Analysis tab has its own Save "
        "settings and Reload settings buttons.");
    ImGui::Spacing();
    if (ImGui::Button("Save & Sync", ImVec2(140.0f, 0.0f))) {
        d.cfg.Domain = d.domainBuf;
        d.cfg.Email = d.emailBuf;
        d.cfg.ApiToken = d.tokenBuf;
        d.cfg.ProjectKey = d.projectKeyBuf;
        d.cfg.TrackerType = d.trackerTypeBuf;
        d.cfg.PlaneUrl = d.planeUrlBuf;
        d.cfg.PlaneWorkspaceSlug = d.planeWorkspaceBuf;
        d.cfg.PlaneProjectId = d.planeProjectBuf;
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
#if defined(SMATCHET_WITH_AI)
        d.cfg.AiApiKey = d.aiApiKeyBuf;
        d.cfg.AiModel = d.aiModelBuf;
        d.cfg.AiBaseUrl = d.aiBaseUrlBuf;
#endif
#if defined(SMATCHET_WITH_MCP)
        d.cfg.McpEnabled = d.mcpEnabled;
        d.cfg.McpPort = d.mcpPort;
        d.cfg.McpAllowRemote = d.mcpAllowRemote;
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
            LOG_INFO("Updated tracker config (Plane). URL='%s', Workspace='%s', Project='%s'", 
                     d.cfg.PlaneUrl.c_str(), d.cfg.PlaneWorkspaceSlug.c_str(), d.cfg.PlaneProjectId.c_str());
        } else {
            LOG_INFO("Updated tracker config (Jira). Domain='%s', Email='%s'", d.cfg.Domain.c_str(), d.cfg.Email.c_str());
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
