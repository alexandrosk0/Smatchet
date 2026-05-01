#include "SmatchetUI.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "Logger.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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
    if (N == 0) {
        return;
    }
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
        CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
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
        if (ImGui::BeginTabItem("Jira")) {
            ImGui::TextUnformatted("Atlassian Cloud");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::InputText("Domain", d.domainBuf, sizeof(d.domainBuf), ImGuiInputTextFlags_CharsNoBlank);
            ImGui::SetItemTooltip("e.g. companyname.atlassian.net");
            ImGui::InputText("Email", d.emailBuf, sizeof(d.emailBuf));
            ImGui::InputText("API Token", d.tokenBuf, sizeof(d.tokenBuf), ImGuiInputTextFlags_Password);
            ImGui::InputText("Project Key", d.projectKeyBuf, sizeof(d.projectKeyBuf),
                             ImGuiInputTextFlags_CharsUppercase);
            ImGui::SetItemTooltip("Used for create meta enrichment, e.g. PROJ");
            ImGui::Spacing();
            ImGui::InputText("New issue: inherit fields from last row", d.newIssueInheritFieldsBuf,
                             sizeof(d.newIssueInheritFieldsBuf));
            ImGui::SetItemTooltip(
                "Comma-separated Jira field ids copied from the last grid row when you click + New issue "
                "(e.g. description, priority, assignee, labels, components). "
                "Summary is never copied from the last row. "
                "Clear and Save & Sync to restore the built-in default list.");
            ImGui::Spacing();
            ImGui::TextWrapped("JQL and column fields are configured in the Views dashboard.");
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
            ImGui::TextDisabled("Changes apply on next host initialization or restart.");
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
            bool jiraBodies = d.cfg.LogJiraHttpBodies;
            if (ImGui::Checkbox("Log Jira HTTP bodies (truncated)", &jiraBodies)) {
                d.cfg.LogJiraHttpBodies = jiraBodies;
                Logger::Instance().SetLogJiraHttpBodies(jiraBodies);
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
        "Save & Sync writes the Jira tab (and optional Assistant / Integrations tabs when enabled in this build) to "
        "disk and refreshes the Jira connection. "
        "Appearance and Diagnostics options save immediately when changed. The Blame Analysis tab has its own Save "
        "settings and Reload settings buttons.");
    ImGui::Spacing();
    if (ImGui::Button("Save & Sync", ImVec2(140.0f, 0.0f))) {
        d.cfg.Domain = d.domainBuf;
        d.cfg.Email = d.emailBuf;
        d.cfg.ApiToken = d.tokenBuf;
        d.cfg.ProjectKey = d.projectKeyBuf;
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
        LOG_INFO("Updated Jira config. Domain='%s', Email='%s'", d.cfg.Domain.c_str(), d.cfg.Email.c_str());
        d.triggerCatalogRefetch = true;
        app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
    }

    ImGui::End();
}
