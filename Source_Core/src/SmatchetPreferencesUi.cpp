#include "SmatchetUI.h"

#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#include "AiClientFactory.h"
#include "AiEndpointSanitize.h"
#include "AiModelCatalog.h"
#include "AiPrefsValidator.h"
#include "AiTypes.h"
#include "IAiClient.h"
#endif

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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(SMATCHET_WITH_MCP)
#include "PluginHost.h"
#endif

#if !defined(SMATCHET_EMBEDDED_IN_UNREAL)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#endif

namespace {

#if !defined(SMATCHET_EMBEDDED_IN_UNREAL)
std::string GetCurrentExePath() {
#if defined(_WIN32)
    char buf[4096] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
#elif defined(__linux__)
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string();
#elif defined(__APPLE__)
    char buf[4096] = {};
    uint32_t sz = sizeof(buf);
    return (_NSGetExecutablePath(buf, &sz) == 0) ? std::string(buf) : std::string();
#else
    return std::string();
#endif
}

bool LaunchDetachedSelf() {
    const std::string exe = GetCurrentExePath();
    if (exe.empty()) {
        return false;
    }
#if defined(_WIN32)
    std::string cmdLine = "\"" + exe + "\"";
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL ok =
        CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
#else
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setsid();
        pid_t pid2 = fork();
        if (pid2 < 0)
            _exit(127);
        if (pid2 == 0) {
            execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#endif
}
#endif // !SMATCHET_EMBEDDED_IN_UNREAL

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

#if defined(SMATCHET_WITH_AI)
// `g_ui` lives in SmatchetUI.cpp. Header declares it `extern` only when
// `SMATCHET_WITH_LUA_AUTOMATION` is defined, so we forward-declare it here
// unconditionally (matches the pattern in AiAssistantController.cpp). The
// Assistant Preferences async probe's MainThreadDispatcher callback reaches
// the global to flip in-flight state + result strings.
extern UiDrawSession g_ui;
#endif

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
#if defined(SMATCHET_WITH_AI)
        // Cancel any in-flight Assistant Preferences probe so its posted callback
        // short-circuits before touching the buffers / cfg. The worker thread
        // itself finishes naturally via `AiClientConfig.TotalTimeoutMs`.
        if (d.assistantPrefsTestCancel) {
            d.assistantPrefsTestCancel->store(true);
        }
        d.assistantPrefsTestInFlight = false;
        d.assistantPrefsTestResult.clear();
        d.assistantPrefsTestResultType = 0;
#endif
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
                MarkPrefsDirty(d);
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
                    MarkPrefsDirty(d);
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
#if defined(SMATCHET_WITH_AI)
        // ----- Assistant tab — explicit Save flow (PR #181 batch 2 + Fix 6/7/8). -----
        //
        // Static InputText buffers live at function scope across frames + tab toggles.
        // The per-frame loop below seeds them from `d.cfg.Ai*` on first paint + on
        // provider change. User typing mutates the buffers only — `d.cfg.Ai*` is
        // untouched until the Save button commits. This replaces the per-keystroke
        // `ConfigManager::Save()` that surfaced the "validation only after typing
        // started" complaint.
        //
        // Layout intent: declared OUTSIDE `if (BeginTabItem)` so the dirty check
        // can compute the tab label suffix ("Assistant *") and the buttons can
        // reach the same buffers from the bottom-strip layout.
        static char s_agentsMdGlobalBuf[1024] = {};
        static char s_projectAgentsMdBuf[1024] = {};
        static bool s_agentsBufsSeeded = false;
        static char s_openAiKeyBuf[1024] = {};
        static char s_anthropicKeyBuf[1024] = {};
        static char s_openAiModelBuf[256] = {};
        static char s_anthropicModelBuf[256] = {};
        static char s_ollamaModelBuf[256] = {};
        static char s_baseUrlBuf[512] = {};
        static char s_ollamaBaseUrlBuf[512] = {};
        static bool s_aiBufsSeeded = false;
        static int s_lastSeededProvider = -1;
        // First-paint seeding from cfg. Also re-seeds whenever provider changes so a
        // user can switch providers and see the persisted-per-provider state rather
        // than the unsaved buffer (matches previous behaviour).
        if (!s_agentsBufsSeeded) {
            s_agentsBufsSeeded = true;
            std::snprintf(s_agentsMdGlobalBuf, sizeof(s_agentsMdGlobalBuf), "%s", d.cfg.AgentsMdGlobalPath.c_str());
            std::snprintf(s_projectAgentsMdBuf, sizeof(s_projectAgentsMdBuf), "%s",
                          d.cfg.ProjectAgentsMdPath.c_str());
        }
        if (!s_aiBufsSeeded || s_lastSeededProvider != d.cfg.AiProviderKind) {
            s_aiBufsSeeded = true;
            s_lastSeededProvider = d.cfg.AiProviderKind;
            std::snprintf(s_openAiKeyBuf, sizeof(s_openAiKeyBuf), "%s", d.cfg.AiApiKey.c_str());
            std::snprintf(s_anthropicKeyBuf, sizeof(s_anthropicKeyBuf), "%s", d.cfg.AiAnthropicApiKey.c_str());
            std::snprintf(s_openAiModelBuf, sizeof(s_openAiModelBuf), "%s", d.cfg.AiModelOpenAi.c_str());
            std::snprintf(s_anthropicModelBuf, sizeof(s_anthropicModelBuf), "%s", d.cfg.AiModelAnthropic.c_str());
            std::snprintf(s_ollamaModelBuf, sizeof(s_ollamaModelBuf), "%s", d.cfg.AiModelOllama.c_str());
            std::snprintf(s_baseUrlBuf, sizeof(s_baseUrlBuf), "%s", d.cfg.AiBaseUrl.c_str());
            std::snprintf(s_ollamaBaseUrlBuf, sizeof(s_ollamaBaseUrlBuf), "%s", d.cfg.AiOllamaBaseUrl.c_str());
        }

        // Reseed-from-cfg helper used by Discard + by the post-commit reseed path.
        // Provider-pinned: the model and base-URL buffers re-read from the cfg
        // values that apply to the CURRENT provider, which is what the user sees.
        auto reseedAssistantBuffersFromCfg = [&]() {
            std::snprintf(s_agentsMdGlobalBuf, sizeof(s_agentsMdGlobalBuf), "%s", d.cfg.AgentsMdGlobalPath.c_str());
            std::snprintf(s_projectAgentsMdBuf, sizeof(s_projectAgentsMdBuf), "%s",
                          d.cfg.ProjectAgentsMdPath.c_str());
            std::snprintf(s_openAiKeyBuf, sizeof(s_openAiKeyBuf), "%s", d.cfg.AiApiKey.c_str());
            std::snprintf(s_anthropicKeyBuf, sizeof(s_anthropicKeyBuf), "%s", d.cfg.AiAnthropicApiKey.c_str());
            std::snprintf(s_openAiModelBuf, sizeof(s_openAiModelBuf), "%s", d.cfg.AiModelOpenAi.c_str());
            std::snprintf(s_anthropicModelBuf, sizeof(s_anthropicModelBuf), "%s", d.cfg.AiModelAnthropic.c_str());
            std::snprintf(s_ollamaModelBuf, sizeof(s_ollamaModelBuf), "%s", d.cfg.AiModelOllama.c_str());
            std::snprintf(s_baseUrlBuf, sizeof(s_baseUrlBuf), "%s", d.cfg.AiBaseUrl.c_str());
            std::snprintf(s_ollamaBaseUrlBuf, sizeof(s_ollamaBaseUrlBuf), "%s", d.cfg.AiOllamaBaseUrl.c_str());
            s_lastSeededProvider = d.cfg.AiProviderKind;
        };

        // Build the working copy with current buffer values applied.
        auto buildAssistantWorkingCopy = [&]() {
            TrackerConfig wc = d.cfg;
            wc.AgentsMdGlobalPath = s_agentsMdGlobalBuf;
            wc.ProjectAgentsMdPath = s_projectAgentsMdBuf;
            wc.AiApiKey = s_openAiKeyBuf;
            wc.AiAnthropicApiKey = s_anthropicKeyBuf;
            wc.AiModelOpenAi = s_openAiModelBuf;
            wc.AiModelAnthropic = s_anthropicModelBuf;
            wc.AiModelOllama = s_ollamaModelBuf;
            wc.AiBaseUrl = s_baseUrlBuf;
            wc.AiOllamaBaseUrl = s_ollamaBaseUrlBuf;
            // AiProviderKind + AgentsMdAutoDiscoverProject are direct-bound below;
            // they live in `d.cfg` already (and are kept in `wc` via the copy ctor).
            return wc;
        };

        // Dirty test — used both for the tab-label suffix and Save/Discard enable.
        const TrackerConfig workingCopyForDirty = buildAssistantWorkingCopy();
        const bool assistantTabDirty =
            workingCopyForDirty.AgentsMdGlobalPath != d.cfg.AgentsMdGlobalPath ||
            workingCopyForDirty.ProjectAgentsMdPath != d.cfg.ProjectAgentsMdPath ||
            workingCopyForDirty.AiApiKey != d.cfg.AiApiKey ||
            workingCopyForDirty.AiAnthropicApiKey != d.cfg.AiAnthropicApiKey ||
            workingCopyForDirty.AiModelOpenAi != d.cfg.AiModelOpenAi ||
            workingCopyForDirty.AiModelAnthropic != d.cfg.AiModelAnthropic ||
            workingCopyForDirty.AiModelOllama != d.cfg.AiModelOllama ||
            workingCopyForDirty.AiBaseUrl != d.cfg.AiBaseUrl ||
            workingCopyForDirty.AiOllamaBaseUrl != d.cfg.AiOllamaBaseUrl;

        const char* assistantTabLabel = assistantTabDirty ? "Assistant *" : "Assistant";
        if (ImGui::BeginTabItem(assistantTabLabel)) {
            // --- Sticky validation banner — rendered FIRST so it stays at the top of
            // the tab regardless of where the user has scrolled. Errors block save;
            // warnings inform but pass through. Validator runs against the
            // working-copy cfg (buffers applied) so the user gets live feedback for
            // the text they're typing, not the last-saved snapshot.
            const TrackerConfig workingCopy = buildAssistantWorkingCopy();
            const smatchet::ai::PrefsValidation validation = smatchet::ai::ValidateAiPrefs(workingCopy);

            auto renderBanner = [&]() {
                if (validation.Errors.empty() && validation.Warnings.empty()) {
                    return;
                }
                const float lineH = ImGui::GetTextLineHeightWithSpacing();
                const float pad = ImGui::GetStyle().FramePadding.y * 2.0f + 4.0f;
                if (!validation.Errors.empty()) {
                    const ImVec4 kErrFill(0.35f, 0.10f, 0.10f, 1.0f);
                    const ImVec4 kErrText(1.0f, 0.55f, 0.55f, 1.0f);
                    // +1 for the header line; bullets compact via line-height.
                    const float h = lineH * static_cast<float>(validation.Errors.size() + 1) + pad;
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, kErrFill);
                    ImGui::BeginChild("##AiPrefsErrorBanner", ImVec2(0.0f, h), true,
                                      ImGuiWindowFlags_NoScrollbar);
                    ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
                    ImGui::TextWrapped("(!) %d error%s block save - fix to enable Save:",
                                       static_cast<int>(validation.Errors.size()),
                                       validation.Errors.size() == 1 ? "" : "s");
                    for (const auto& e : validation.Errors) {
                        ImGui::BulletText("%s", e.c_str());
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
                if (!validation.Warnings.empty()) {
                    const ImVec4 kWarnFill(0.32f, 0.26f, 0.06f, 1.0f);
                    const ImVec4 kWarnText(1.0f, 0.90f, 0.45f, 1.0f);
                    const float h = lineH * static_cast<float>(validation.Warnings.size() + 1) + pad;
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, kWarnFill);
                    ImGui::BeginChild("##AiPrefsWarnBanner", ImVec2(0.0f, h), true,
                                      ImGuiWindowFlags_NoScrollbar);
                    ImGui::PushStyleColor(ImGuiCol_Text, kWarnText);
                    ImGui::TextWrapped("Warnings (Save still proceeds):");
                    for (const auto& w : validation.Warnings) {
                        ImGui::BulletText("%s", w.c_str());
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
            };
            renderBanner();

            // Map a field key to its per-issue glyph rendered next to the input. Pure
            // display — no state. The most-severe issue for the field wins.
            auto fieldIssueFor = [&](const char* key) -> const smatchet::ai::PrefsValidationIssue* {
                if (key == nullptr || *key == '\0') {
                    return nullptr;
                }
                const smatchet::ai::PrefsValidationIssue* found = nullptr;
                for (const auto& issue : validation.Issues) {
                    if (issue.FieldKey != key) {
                        continue;
                    }
                    if (found == nullptr) {
                        found = &issue;
                        continue;
                    }
                    // Promote to Error if a later Error appears for the same field.
                    if (issue.Severity == smatchet::ai::PrefsSeverity::Error &&
                        found->Severity == smatchet::ai::PrefsSeverity::Warning) {
                        found = &issue;
                    }
                }
                return found;
            };
            auto renderFieldGlyph = [&](const char* fieldKey) {
                const smatchet::ai::PrefsValidationIssue* issue = fieldIssueFor(fieldKey);
                if (issue == nullptr) {
                    return;
                }
                const bool isError = (issue->Severity == smatchet::ai::PrefsSeverity::Error);
                const ImVec4 col = isError ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                                           : ImVec4(1.0f, 0.85f, 0.30f, 1.0f);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(isError ? "(!)" : "(~)");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", issue->Message.c_str());
                }
            };

            // Any field edit invalidates a stale Test-connection result.
            auto clearStaleTestResult = [&]() {
                if (!d.assistantPrefsTestInFlight) {
                    d.assistantPrefsTestResult.clear();
                    d.assistantPrefsTestResultType = 0;
                }
            };

            ImGui::TextUnformatted("agents.md harness");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextWrapped(
                "agents.md files supply per-project + global instructions injected into every Assistant turn. "
                "The global layer defaults to %%LOCALAPPDATA%%/Smatchet/agents.md when blank; the project layer is "
                "explicit via the Project path field below. Each layer is capped at 64 KB.");
            ImGui::Spacing();

            // Buffers seeded once at function scope above; mutations stay buffer-local
            // until the Save button commits + ConfigManager::Save persists. The user
            // sees `(!)` / `(~)` glyphs + the banner above for live feedback.
            const bool globalChanged =
                ImGui::InputText("Global agents.md path", s_agentsMdGlobalBuf, sizeof(s_agentsMdGlobalBuf));
            ImGui::SetItemTooltip("Default location is %LOCALAPPDATA%/Smatchet/agents.md (resolved at load time). "
                                  "Override to point at a checked-in shared file if your team keeps one.");
            const bool projectChanged = ImGui::InputText("Project agents.md path (optional override)",
                                                         s_projectAgentsMdBuf, sizeof(s_projectAgentsMdBuf));
            ImGui::SetItemTooltip("When set, this exact path is used as the project layer. Leave blank to disable "
                                  "the project layer entirely unless 'Auto-discover' is enabled below.");
            // Auto-discover bound directly to cfg — single bool flag, not a credential.
            // Same boat as the (later in tab) verify-on-save checkbox: meta-setting,
            // not a value users compose-and-review before saving, so direct bind +
            // immediate persistence is the right UX.
            bool autoDiscover = d.cfg.AgentsMdAutoDiscoverProject;
            const bool autoDiscoverChanged = ImGui::Checkbox("Auto-discover project agents.md (walk up from cwd)",
                                                              &autoDiscover);
            ImGui::SetItemTooltip("When OFF (default), only the Global file and the explicit Project path above are "
                                  "used. Smatchet's own AGENTS.md and any unrelated parent-repo AGENTS.md will not "
                                  "be picked up automatically. Turn ON for old behavior (walks up the cwd chain).");
            if (autoDiscoverChanged) {
                d.cfg.AgentsMdAutoDiscoverProject = autoDiscover;
                MarkPrefsDirty(d);
                // Invalidate the worker-side agents.md cache so the next turn
                // re-reads from disk instead of serving the stale blob.
                if (app.HasAiAssistantController()) {
                    app.GetAiAssistantController().InvalidateAgentsMdCache();
                }
            }
            if (globalChanged || projectChanged) {
                clearStaleTestResult();
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Provider");
            ImGui::Spacing();

            // Provider Combo — enumerated via AiClientFactory so display order stays
            // stable across builds and matches the AiProvider enum (load-bearing for
            // round-tripping persisted AiProviderKind int).
            const std::vector<AiClientFactory::ProviderEntry> providers = AiClientFactory::EnumeratedProviders();
            std::vector<const char*> providerLabels;
            providerLabels.reserve(providers.size());
            std::transform(providers.begin(), providers.end(), std::back_inserter(providerLabels),
                           [](const AiClientFactory::ProviderEntry& p) { return p.Display.c_str(); });
            int providerIdx = 0;
            auto providerIt =
                std::find_if(providers.begin(), providers.end(), [&](const AiClientFactory::ProviderEntry& p) {
                    return static_cast<int>(p.Kind) == d.cfg.AiProviderKind;
                });
            if (providerIt != providers.end()) {
                providerIdx = static_cast<int>(std::distance(providers.begin(), providerIt));
            }
            if (ImGui::Combo("AI provider", &providerIdx, providerLabels.data(),
                             static_cast<int>(providerLabels.size()))) {
                d.cfg.AiProviderKind = static_cast<int>(providers[providerIdx].Kind);
                MarkPrefsDirty(d);
                // Re-seed so the per-provider buffers below load the chosen provider's
                // persisted state on the next paint. The check at function-scope
                // already runs `s_lastSeededProvider != d.cfg.AiProviderKind` and
                // refreshes — no extra work needed here.
                clearStaleTestResult();
            }
            const AiProvider selectedKind = providers[providerIdx].Kind;

            ImGui::Spacing();
            if (selectedKind == AiProvider::OpenAi || selectedKind == AiProvider::OllamaOpenAiCompat) {
                const bool isLocalCompat = (selectedKind == AiProvider::OllamaOpenAiCompat);
                const char* keyLabel = isLocalCompat ? "API key (optional for local servers)"
                                                     : "OpenAI / compat API key";
                const bool keyChanged = ImGui::InputText(keyLabel, s_openAiKeyBuf,
                                                         sizeof(s_openAiKeyBuf), ImGuiInputTextFlags_Password);
                ImGui::SetItemTooltip(
                    isLocalCompat
                        ? "Local OpenAI-compatible servers (LM Studio / Ollama / LocalAI / vLLM) usually accept any "
                          "value or no key at all. Leave blank if your server is unauthenticated."
                        : "OpenAI / Azure / Groq / Together key. Stored DPAPI-protected at rest "
                          "(Windows); plaintext fallback on other OS.");
                renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiApiKey);
                // Model dropdown when the provider has a published catalog.
                // Falls back to a free-form InputText otherwise (OllamaOpenAi
                // surfaces here, but its models are local + user-defined).
                bool modelChanged = false;
                const std::vector<smatchet::ai::ModelOption> catalog = smatchet::ai::KnownModels(selectedKind);
                if (!catalog.empty()) {
                    std::vector<std::string> displays;
                    displays.reserve(catalog.size());
                    for (const auto& m : catalog) {
                        displays.push_back(m.DisplayName);
                    }
                    std::vector<const char*> displayPtrs;
                    displayPtrs.reserve(displays.size());
                    std::transform(displays.begin(), displays.end(), std::back_inserter(displayPtrs),
                                   [](const std::string& s) { return s.c_str(); });
                    int selectedIdx = -1;
                    auto it = std::find_if(catalog.begin(), catalog.end(), [&](const smatchet::ai::ModelOption& m) {
                        return m.Id == s_openAiModelBuf;
                    });
                    if (it != catalog.end()) {
                        selectedIdx = static_cast<int>(std::distance(catalog.begin(), it));
                    }
                    int comboIdx = (selectedIdx >= 0) ? selectedIdx : 0;
                    const char* modelLabel = isLocalCompat ? "Model (local server name)" : "OpenAI model";
                    if (ImGui::Combo(modelLabel, &comboIdx, displayPtrs.data(),
                                     static_cast<int>(displayPtrs.size()))) {
                        std::snprintf(s_openAiModelBuf, sizeof(s_openAiModelBuf), "%s",
                                      catalog[static_cast<std::size_t>(comboIdx)].Id.c_str());
                        modelChanged = true;
                    }
                    renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiModelOpenAi);
                    if (selectedIdx < 0 && s_openAiModelBuf[0] != '\0') {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(custom: %s)", s_openAiModelBuf);
                    }
                    if (ImGui::CollapsingHeader("Custom model ID (advanced)")) {
                        if (ImGui::InputText("##openai_custom", s_openAiModelBuf, sizeof(s_openAiModelBuf))) {
                            modelChanged = true;
                        }
                    }
                } else {
                    const char* modelLabel = isLocalCompat ? "Model (local server name)" : "OpenAI model";
                    modelChanged = ImGui::InputText(modelLabel, s_openAiModelBuf, sizeof(s_openAiModelBuf));
                    ImGui::SetItemTooltip(
                        isLocalCompat
                            ? "Free-form: the model name your local server reports. For LM Studio this is the "
                              "loaded file (e.g. 'meta-llama/Llama-3.2-3B-Instruct'); for Ollama use the name "
                              "from 'ollama list'."
                            : "OpenAI-compatible model identifier.");
                    renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiModelOpenAi);
                }
                bool baseChanged = false;
                if (isLocalCompat) {
                    baseChanged = ImGui::InputTextWithHint(
                        "Base URL", "http://localhost:1234/v1 (LM Studio)", s_baseUrlBuf, sizeof(s_baseUrlBuf));
                    ImGui::SetItemTooltip(
                        "Local OpenAI-compatible endpoint. LM Studio defaults to http://localhost:1234/v1; Ollama's "
                        "compat endpoint is http://localhost:11434/v1; LocalAI / vLLM expose their own. The "
                        "/v1/chat/completions path is appended automatically.");
                } else {
                    baseChanged = ImGui::InputText("Base URL (optional)", s_baseUrlBuf, sizeof(s_baseUrlBuf));
                    ImGui::SetItemTooltip(
                        "Leave blank for https://api.openai.com. Override for Azure / Groq / Together endpoints. "
                        "The /v1/chat/completions path is appended.");
                }
                renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiBaseUrl);
                if (keyChanged || modelChanged || baseChanged) {
                    // Buffer-only — Save button commits.
                    clearStaleTestResult();
                }
            } else if (selectedKind == AiProvider::Anthropic) {
                const bool keyChanged = ImGui::InputText("Anthropic API key", s_anthropicKeyBuf,
                                                         sizeof(s_anthropicKeyBuf), ImGuiInputTextFlags_Password);
                ImGui::SetItemTooltip(
                    "Anthropic API key (sk-ant-...). Stored DPAPI-protected at rest (Windows); plaintext fallback "
                    "on other OS.");
                renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiAnthropicApiKey);
                bool modelChanged = false;
                const std::vector<smatchet::ai::ModelOption> catalog = smatchet::ai::KnownModels(AiProvider::Anthropic);
                if (!catalog.empty()) {
                    std::vector<std::string> displays;
                    displays.reserve(catalog.size());
                    for (const auto& m : catalog) {
                        displays.push_back(m.DisplayName);
                    }
                    std::vector<const char*> displayPtrs;
                    displayPtrs.reserve(displays.size());
                    std::transform(displays.begin(), displays.end(), std::back_inserter(displayPtrs),
                                   [](const std::string& s) { return s.c_str(); });
                    int selectedIdx = -1;
                    auto it = std::find_if(catalog.begin(), catalog.end(), [&](const smatchet::ai::ModelOption& m) {
                        return m.Id == s_anthropicModelBuf;
                    });
                    if (it != catalog.end()) {
                        selectedIdx = static_cast<int>(std::distance(catalog.begin(), it));
                    }
                    int comboIdx = (selectedIdx >= 0) ? selectedIdx : 0;
                    if (ImGui::Combo("Anthropic model", &comboIdx, displayPtrs.data(),
                                     static_cast<int>(displayPtrs.size()))) {
                        std::snprintf(s_anthropicModelBuf, sizeof(s_anthropicModelBuf), "%s",
                                      catalog[static_cast<std::size_t>(comboIdx)].Id.c_str());
                        modelChanged = true;
                    }
                    renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiModelAnthropic);
                    if (selectedIdx < 0 && s_anthropicModelBuf[0] != '\0') {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(custom: %s)", s_anthropicModelBuf);
                    }
                    if (ImGui::CollapsingHeader("Custom Anthropic model ID (advanced)")) {
                        if (ImGui::InputText("##anthropic_custom", s_anthropicModelBuf, sizeof(s_anthropicModelBuf))) {
                            modelChanged = true;
                        }
                    }
                } else {
                    modelChanged =
                        ImGui::InputText("Anthropic model", s_anthropicModelBuf, sizeof(s_anthropicModelBuf));
                    ImGui::SetItemTooltip("Anthropic model identifier.");
                    renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiModelAnthropic);
                }
                const bool baseChanged = ImGui::InputText("Base URL (optional)", s_baseUrlBuf, sizeof(s_baseUrlBuf));
                ImGui::SetItemTooltip("Leave blank for https://api.anthropic.com. Override only if you're routing "
                                      "through an Anthropic-compatible proxy.");
                renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiBaseUrl);
                if (keyChanged || modelChanged || baseChanged) {
                    // Buffer-only — Save button commits.
                    clearStaleTestResult();
                }
            } else if (selectedKind == AiProvider::OllamaNative) {
                ImGui::TextDisabled("Ollama is local — no API key required.");
                const bool modelChanged = ImGui::InputText("Ollama model", s_ollamaModelBuf, sizeof(s_ollamaModelBuf));
                ImGui::SetItemTooltip("Locally-installed model name. e.g. llama3, qwen2.5, mistral, "
                                      "deepseek-r1. Run `ollama list` to see what's installed.");
                renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiModelOllama);
                const bool baseChanged =
                    ImGui::InputText("Ollama base URL", s_ollamaBaseUrlBuf, sizeof(s_ollamaBaseUrlBuf));
                ImGui::SetItemTooltip(
                    "Default http://localhost:11434. Override for a remote Ollama daemon on the LAN.");
                renderFieldGlyph(smatchet::ai::PrefsFieldKey::kAiOllamaBaseUrl);
                if (modelChanged || baseChanged) {
                    // Buffer-only — Save button commits.
                    clearStaleTestResult();
                }
            }

            // --- Save / Discard / Test connection strip + verify-on-save checkbox. ---
            //
            // Save is the discrete commit moment that replaces the per-keystroke
            // `ConfigManager::Save()` autosave. Test-connection runs the
            // long-deferred async `ProbeReachability` worker; the same worker is
            // re-used by save-with-verify when the checkbox is on (default).
            //
            // Threading invariant: the worker thread captures a value-copy of
            // `workingCopy` + a shared_ptr cancel atom + the
            // `app.mainThreadDispatcher` reference. It constructs a fresh
            // `IAiClient`, runs `ProbeReachability`, then posts the result via
            // MainThreadDispatcher. The worker never touches `g_ui` directly.
            //
            // Cancel-on-close (handled below the tab body via the bottom-of-
            // function reset) sets `*assistantPrefsTestCancel = true` so the
            // posted callback short-circuits if Preferences closes mid-probe.

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Shared probe helper — runs ProbeReachability on a worker thread and
            // posts the result back via MainThreadDispatcher. Captured by value
            // into the lambda so a concurrent edit during the probe doesn't
            // change the bytes the worker sees.
            //
            // `saveOnSuccess`: when true, the success callback also commits the
            // working-copy state to `d.cfg` + persists via `ConfigManager::Save`.
            // Used by the verify-on-save Save flow; the plain Test button passes
            // false (display-only).
            auto runProbe = [&app, &d](TrackerConfig probeCfg, AiProvider provider, bool saveOnSuccess) {
                d.assistantPrefsTestInFlight = true;
                d.assistantPrefsTestResult = "Testing...";
                d.assistantPrefsTestResultType = 0;
                d.assistantPrefsTestCancel = std::make_shared<std::atomic<bool>>(false);
                d.assistantPrefsTestProbeSavesOnSuccess = saveOnSuccess;
                d.assistantPrefsSavePendingCfg = probeCfg;
                auto cancel = d.assistantPrefsTestCancel;
                // Provider-aware ApiKey / BaseUrl pick. Mirrors `BuildClientConfig`
                // in `AiAssistantController.cpp` (kept local because that helper
                // lives in an anonymous namespace).
                std::string apiKey;
                std::string baseUrl;
                switch (provider) {
                case AiProvider::Anthropic:
                    apiKey = probeCfg.AiAnthropicApiKey;
                    baseUrl = probeCfg.AiBaseUrl;
                    break;
                case AiProvider::OllamaNative:
                    apiKey.clear();
                    baseUrl = probeCfg.AiOllamaBaseUrl;
                    break;
                case AiProvider::OllamaOpenAiCompat:
                    apiKey = probeCfg.AiApiKey;
                    baseUrl = probeCfg.AiBaseUrl.empty() ? probeCfg.AiOllamaBaseUrl : probeCfg.AiBaseUrl;
                    break;
                case AiProvider::OpenAi:
                default:
                    apiKey = probeCfg.AiApiKey;
                    baseUrl = probeCfg.AiBaseUrl;
                    break;
                }
                // Strip header-smuggling control characters from the key (cheap
                // defence-in-depth; libcurl rejects them too).
                std::string sanitisedKey;
                sanitisedKey.reserve(apiKey.size());
                for (char c : apiKey) {
                    if (c != '\r' && c != '\n' && c != '\0') {
                        sanitisedKey.push_back(c);
                    }
                }
                // Validate BaseUrl via the shared sanitiser; empty falls back to
                // provider default.
                std::string sanitisedBase;
                if (!baseUrl.empty()) {
                    std::string normalised;
                    const smatchet::ai::pure::EndpointVerdict v =
                        smatchet::ai::pure::SanitizeAiEndpointUrl(baseUrl, normalised);
                    if (v == smatchet::ai::pure::EndpointVerdict::Allowed) {
                        sanitisedBase = normalised;
                    } else {
                        LOG_WARN("Preferences: Test connection — endpoint URL %s; falling back to provider default.",
                                 smatchet::ai::pure::EndpointVerdictDescription(v));
                    }
                }
                AiClientConfig clientCfg;
                clientCfg.ApiKey = sanitisedKey;
                clientCfg.BaseUrl = sanitisedBase;
                clientCfg.ConnectTimeoutMs = 5000;
                // Tight cap on the probe — much shorter than the chat default so
                // a hung backend doesn't keep the spinner up for two minutes.
                clientCfg.TotalTimeoutMs = 15000;

                MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;

                std::thread([provider, clientCfg, saveOnSuccess, probeCfg, cancel, &dispatcher]() {
                    std::string errMsg;
                    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
                    if (!client) {
                        errMsg = "Provider not available in this build.";
                    } else {
                        errMsg = client->ProbeReachability(clientCfg);
                    }
                    dispatcher.PostToMainThread([provider, errMsg, cancel, saveOnSuccess, probeCfg]() {
                        if (cancel && cancel->load()) {
                            return;
                        }
                        g_ui.assistantPrefsTestInFlight = false;
                        const bool ok = errMsg.empty();
                        if (ok) {
                            if (saveOnSuccess) {
                                g_ui.cfg = probeCfg;
                                ConfigManager::Save(g_ui.cfg);
                                g_ui.assistantPrefsTestResult = "Saved + verified.";
                                g_ui.assistantPrefsTestResultType = 1;
                                SmatchetToastManager::Instance().Push(
                                    std::string("Preferences"),
                                    std::string("Saved + verified."),
                                    ToastType::Success, 3500);
                            } else {
                                g_ui.assistantPrefsTestResult = "Verified.";
                                g_ui.assistantPrefsTestResultType = 1;
                            }
                        } else {
                            g_ui.assistantPrefsTestResult = std::string("Failed: ") + errMsg;
                            g_ui.assistantPrefsTestResultType = 2;
                            if (saveOnSuccess) {
                                SmatchetToastManager::Instance().Push(
                                    std::string("Preferences"),
                                    std::string("Connection failed - not saved: ") + errMsg,
                                    ToastType::Error, 6000);
                            }
                        }
                        // Avoid leaking the cfg snapshot past the callback.
                        g_ui.assistantPrefsSavePendingCfg = TrackerConfig();
                        g_ui.assistantPrefsTestProbeSavesOnSuccess = false;
                        (void)provider;
                    });
                }).detach();
            };

            // --- Save / Discard buttons row ---
            const bool canSave = assistantTabDirty && !d.assistantPrefsTestInFlight && validation.IsOk();
            const bool canDiscard = assistantTabDirty && !d.assistantPrefsTestInFlight;

            if (!canSave) {
                ImGui::BeginDisabled(true);
            }
            const bool savePressed = ImGui::Button("Save changes");
            if (!canSave) {
                ImGui::EndDisabled();
                if (assistantTabDirty && !validation.IsOk() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Fix the errors above to enable Save.");
                }
            }
            ImGui::SameLine();
            if (!canDiscard) {
                ImGui::BeginDisabled(true);
            }
            const bool discardPressed = ImGui::Button("Discard");
            if (!canDiscard) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            // Tab-bar dirty hint also rendered inline for users who hide the tab strip.
            if (assistantTabDirty) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "unsaved changes");
            } else {
                ImGui::TextDisabled("no unsaved changes");
            }

            if (savePressed && canSave) {
                if (d.cfg.AiPrefsVerifyOnSave) {
                    // Kick off the probe with saveOnSuccess=true. The posted
                    // callback commits + toasts on success, surfaces an error
                    // toast on failure.
                    runProbe(workingCopy, selectedKind, /*saveOnSuccess=*/true);
                } else {
                    // Static-validation-only path: commit immediately. AI Assistant tab keeps an
                    // explicit synchronous Save (PR #181 / #184) — does NOT route through the
                    // SmatchetPreferences debounce since the toast announces persistence and the
                    // user expects on-disk state when the Save button releases.
                    d.cfg = workingCopy;
                    ConfigManager::Save(d.cfg);
                    SmatchetToastManager::Instance().Push(
                        std::string("Preferences"),
                        std::string("Saved (not verified)."),
                        ToastType::Success, 3500);
                    // Invalidate agents.md cache in case the global / project path changed.
                    if (app.HasAiAssistantController()) {
                        app.GetAiAssistantController().InvalidateAgentsMdCache();
                    }
                }
            }
            if (discardPressed && canDiscard) {
                reseedAssistantBuffersFromCfg();
                d.assistantPrefsTestResult.clear();
                d.assistantPrefsTestResultType = 0;
                SmatchetToastManager::Instance().Push(
                    std::string("Preferences"),
                    std::string("Reverted to last saved values."),
                    ToastType::Info, 3000);
            }

            // --- Verify-on-save checkbox (direct-bound; meta-setting) ---
            // AI Assistant tab keeps explicit synchronous Save (sibling to lines 953 / 1021).
            if (ImGui::Checkbox("Verify connection on save", &d.cfg.AiPrefsVerifyOnSave)) {
                ConfigManager::Save(d.cfg);
            }
            ImGui::SetItemTooltip("When checked (default), Save runs a connection probe before committing. "
                                  "Uncheck to save offline-entered credentials without a live check.");

            // --- Test connection button + result line ---
            ImGui::Spacing();
            // Disable when validation fails, when a probe is already running, or
            // for local-only providers that have no target URL configured.
            bool canTest = validation.IsOk() && !d.assistantPrefsTestInFlight;
            const bool isLocalProvider =
                (selectedKind == AiProvider::OllamaNative || selectedKind == AiProvider::OllamaOpenAiCompat);
            const std::string testBaseUrl =
                (selectedKind == AiProvider::OllamaNative)
                    ? std::string(s_ollamaBaseUrlBuf)
                    : (s_baseUrlBuf[0] != '\0' ? std::string(s_baseUrlBuf)
                                               : (isLocalProvider ? std::string(s_ollamaBaseUrlBuf) : std::string()));
            if (isLocalProvider && testBaseUrl.empty()) {
                canTest = false;
            }
            if (!canTest) {
                ImGui::BeginDisabled(true);
            }
            const bool testPressed = ImGui::Button("Test connection");
            if (!canTest) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) {
                    if (d.assistantPrefsTestInFlight) {
                        ImGui::SetTooltip("Probe already running...");
                    } else if (!validation.IsOk()) {
                        ImGui::SetTooltip("Fix the errors above to enable Test connection.");
                    } else if (isLocalProvider && testBaseUrl.empty()) {
                        ImGui::SetTooltip("Set a base URL above first (local servers need an explicit endpoint).");
                    }
                }
            }
            if (testPressed && canTest) {
                runProbe(workingCopy, selectedKind, /*saveOnSuccess=*/false);
            }
            ImGui::SameLine();
            if (!d.assistantPrefsTestResult.empty()) {
                const int kind = d.assistantPrefsTestResultType;
                ImVec4 col(0.78f, 0.78f, 0.78f, 1.0f);
                if (kind == 1) {
                    col = ImVec4(0.45f, 0.95f, 0.55f, 1.0f);
                } else if (kind == 2) {
                    col = ImVec4(1.0f, 0.55f, 0.55f, 1.0f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(d.assistantPrefsTestResult.c_str());
                ImGui::PopStyleColor();
                if (kind == 2 && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", d.assistantPrefsTestResult.c_str());
                }
            }

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

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings storage location");
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
            ImGui::TextWrapped("Plugin default: writable files (config / views / SQLite cache / ImGui layout) live in "
                               "<UnrealProject>/Saved next to the runtime cache. Switch to Shared when the project dir "
                               "is read-only (source-controlled, network share, sandboxed runner) and Smatchet should "
                               "instead use your OS user-data folder. Change takes effect on next launch.");
#else
            ImGui::TextWrapped("Standalone default: writable files live in your OS user-data folder, shared across "
                               "exes / installs. Switch to Portable to keep all writable files next to the executable "
                               "instead — useful when running from a thumb drive or testing parallel builds. Change "
                               "takes effect on next launch.");
#endif
            const std::string runtimeAssetDir = ConfigManager::GetRuntimeAssetDirectory();
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
            constexpr ConfigManager::StoragePreference kDefaultPref = ConfigManager::StoragePreference::Portable;
#else
            constexpr ConfigManager::StoragePreference kDefaultPref = ConfigManager::StoragePreference::Shared;
#endif
            const ConfigManager::StoragePreference currentPref =
                ConfigManager::GetStoragePreference(runtimeAssetDir, kDefaultPref);
            int prefIndex = (currentPref == ConfigManager::StoragePreference::Portable) ? 0 : 1;
            const char* items[] = {"Portable (next to runtime files)", "Shared (OS user-data folder)"};
            static bool s_storageModeChanged = false;
            if (ImGui::Combo("Storage mode", &prefIndex, items, IM_ARRAYSIZE(items))) {
                const ConfigManager::StoragePreference chosen = (prefIndex == 0)
                                                                    ? ConfigManager::StoragePreference::Portable
                                                                    : ConfigManager::StoragePreference::Shared;
                std::string err;
                if (ConfigManager::SetStoragePreference(runtimeAssetDir, chosen, err)) {
                    s_storageModeChanged = true;
                    SmatchetToastManager::Instance().Push(
                        std::string("Storage"),
                        std::string(chosen == ConfigManager::StoragePreference::Portable
                                        ? "Storage mode set to Portable. Restart Smatchet for the new "
                                          "writable-files location to take effect."
                                        : "Storage mode set to Shared. Restart Smatchet for the new "
                                          "writable-files location to take effect."),
                        ToastType::Info, 6000);
                } else {
                    SmatchetToastManager::Instance().Push(std::string("Storage"),
                                                          err.empty() ? std::string("Could not write storage-mode "
                                                                                    "marker file.")
                                                                      : err,
                                                          ToastType::Error, 6000);
                }
            }
            if (s_storageModeChanged) {
                ImGui::Spacing();
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
                ImGui::TextWrapped("Restart Unreal Editor for the storage-mode change to take effect.");
#else
                if (ImGui::Button("Restart Smatchet now")) {
                    if (LaunchDetachedSelf()) {
                        s_storageModeChanged = false;
                        app.RequestAppQuit();
                    } else {
                        SmatchetToastManager::Instance().Push(
                            std::string("Storage"),
                            std::string("Could not relaunch Smatchet — exit and restart manually."), ToastType::Error,
                            6000);
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(spawns a new instance and exits this one)");
#endif
            }
            ImGui::TextDisabled("Current writable directory: %s", ConfigManager::GetUserDataDirectory().c_str());
            ImGui::TextDisabled("Marker file: %s",
                                ConfigManager::GetStoragePreferenceFlagPath(runtimeAssetDir).c_str());

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
                MarkPrefsDirty(d);
                SmatchetRequestFontReload(d.cfg.SelectedFontName, static_cast<float>(d.cfg.FontSizePt));
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
                    MarkPrefsDirty(d);
                    SmatchetLocalization::SetLanguage(d.cfg.UiLanguage);
                }
            }
            ImGui::SetItemTooltip(
                "Select the UI language. App-owned UI text changes immediately; tracker data is shown as-is.");

            ImGui::Spacing();
            ImGui::TextUnformatted("Grid and field text");
            ImGui::Separator();
            if (ImGui::Checkbox("Show tooltips when text overflows", &d.cfg.EnableFieldOverflowTooltips)) {
                MarkPrefsDirty(d);
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
                MarkPrefsDirty(d);
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
                MarkPrefsDirty(d);
            }
            ImGui::SetItemTooltip("Select how date and datetime values are rendered in the grids and UI panels.");

            if (currentDateFormatIdx == 0) {
                int threshold = d.cfg.DateCompactRelativeThresholdDays;
                if (ImGui::SliderInt("Compact Relative Threshold (Days)", &threshold, 1, 90, "%d days")) {
                    d.cfg.DateCompactRelativeThresholdDays = threshold;
                    MarkPrefsDirty(d);
                }
                ImGui::SetItemTooltip("Threshold in days where the compact view transitions from relative (e.g. -3d) "
                                      "to short absolute (e.g. May 07 '26).");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Updates");
            ImGui::Separator();
            if (ImGui::Checkbox("Check for updates automatically", &d.cfg.UpdateCheckEnabled)) {
                MarkPrefsDirty(d);
            }
            if (ImGui::Checkbox("Include prerelease builds", &d.cfg.UpdateIncludePrerelease)) {
                MarkPrefsDirty(d);
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
                    MarkPrefsDirty(d);
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
                                MarkPrefsDirty(d);
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
                                MarkPrefsDirty(d);
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
                            MarkPrefsDirty(d);
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
                            MarkPrefsDirty(d);
                        }

                        ImGui::SameLine(280.0f);
                        ImGui::TextUnformatted("ID:");
                        ImGui::SameLine(310.0f);
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::InputText("##EditQuickId", idBuf, sizeof(idBuf))) {
                            t.Id = idBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            MarkPrefsDirty(d);
                        }

                        ImGui::TextUnformatted("Body:");
                        if (ImGui::InputTextMultiline("##EditQuickText", textBuf, sizeof(textBuf),
                                                      ImVec2(-FLT_MIN, 60.0f))) {
                            t.Text = textBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            MarkPrefsDirty(d);
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
                        MarkPrefsDirty(d);
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Annotate Comments")) {
                    ImGui::TextUnformatted("Annotate Quick Comments");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize templates displayed when clicking on the Annotate rows. "
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
                                MarkPrefsDirty(d);
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
                                MarkPrefsDirty(d);
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
                            MarkPrefsDirty(d);
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
                            MarkPrefsDirty(d);
                        }

                        ImGui::SameLine(280.0f);
                        ImGui::TextUnformatted("ID:");
                        ImGui::SameLine(310.0f);
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::InputText("##EditBlameId", idBuf, sizeof(idBuf))) {
                            t.Id = idBuf;
                            d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                            MarkPrefsDirty(d);
                        }

                        ImGui::TextUnformatted("Body:");
                        if (ImGui::InputTextMultiline("##EditBlameText", textBuf, sizeof(textBuf),
                                                      ImVec2(-FLT_MIN, 60.0f))) {
                            t.Text = textBuf;
                            d.cfg.BlameCommentTemplates = s_blameTemplatesList;
                            MarkPrefsDirty(d);
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
                        MarkPrefsDirty(d);
                    }

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Annotate")) {
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

        MarkPrefsDirty(d);
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
