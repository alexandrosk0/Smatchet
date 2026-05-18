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

#if defined(SMATCHET_WITH_WHISPER)
#include "HotkeyParse.h"
#include "MainThreadDispatcher.h"
#include "ModelCatalog.h"
#include "ModelDownloader.h"
#include "SmatchetWhisperSetupBanner.h"
#include "ModelCatalog.h"
#include "WavWriter.h"
#include "WhisperApiClient.h"
#include "WhisperLocal.h"
#include "WindowsAudioCapture.h"
#include "WhisperApiKeyResolve.h"
#include "WhisperConsentGate.h"
#include "WhisperPlugin.h"
#include <cpr/cpr.h>
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
        // ----- Assistant tab — minimal config. -----
        //
        // Goal: "select provider from dropdown + press Test connection" Just Works
        // for LM Studio out of the box. Per-provider sensible defaults are seeded
        // on provider switch. Field edits commit through MarkPrefsDirty (debounced
        // ~100 ms save). No explicit Save button — the validator banner + the
        // Test-connection result line provide all the feedback.
        //
        // Static InputText buffers live at function scope across frames + tab
        // toggles. Re-seeded from `d.cfg.Ai*` on first paint + on provider change.
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
        if (!s_agentsBufsSeeded) {
            s_agentsBufsSeeded = true;
            std::snprintf(s_agentsMdGlobalBuf, sizeof(s_agentsMdGlobalBuf), "%s", d.cfg.AgentsMdGlobalPath.c_str());
            std::snprintf(s_projectAgentsMdBuf, sizeof(s_projectAgentsMdBuf), "%s", d.cfg.ProjectAgentsMdPath.c_str());
        }
        // Reseed on first paint, on provider switch, or when the Test-connection
        // success callback persisted a fallback default value back into cfg (so
        // the buffer reflects the just-saved value on the next paint).
        if (!s_aiBufsSeeded || s_lastSeededProvider != d.cfg.AiProviderKind || d.assistantPrefsForceBufferReseed) {
            s_aiBufsSeeded = true;
            s_lastSeededProvider = d.cfg.AiProviderKind;
            d.assistantPrefsForceBufferReseed = false;
            std::snprintf(s_openAiKeyBuf, sizeof(s_openAiKeyBuf), "%s", d.cfg.AiApiKey.c_str());
            std::snprintf(s_anthropicKeyBuf, sizeof(s_anthropicKeyBuf), "%s", d.cfg.AiAnthropicApiKey.c_str());
            std::snprintf(s_openAiModelBuf, sizeof(s_openAiModelBuf), "%s", d.cfg.AiModelOpenAi.c_str());
            std::snprintf(s_anthropicModelBuf, sizeof(s_anthropicModelBuf), "%s", d.cfg.AiModelAnthropic.c_str());
            std::snprintf(s_ollamaModelBuf, sizeof(s_ollamaModelBuf), "%s", d.cfg.AiModelOllama.c_str());
            std::snprintf(s_baseUrlBuf, sizeof(s_baseUrlBuf), "%s", d.cfg.AiBaseUrl.c_str());
            std::snprintf(s_ollamaBaseUrlBuf, sizeof(s_ollamaBaseUrlBuf), "%s", d.cfg.AiOllamaBaseUrl.c_str());
        }

        if (ImGui::BeginTabItem("Assistant")) {
            // --- Sticky validation banner — rendered FIRST so it stays at the top
            // of the tab. Validator runs against the live cfg (auto-saved on every
            // field edit) so the user gets live feedback for the text they're
            // typing.
            const smatchet::ai::PrefsValidation validation = smatchet::ai::ValidateAiPrefs(d.cfg);

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
                    ImGui::BeginChild("##AiPrefsErrorBanner", ImVec2(0.0f, h), true, ImGuiWindowFlags_NoScrollbar);
                    ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
                    ImGui::TextWrapped(
                        "(!) %d configuration error%s - see details below:", static_cast<int>(validation.Errors.size()),
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
                    ImGui::BeginChild("##AiPrefsWarnBanner", ImVec2(0.0f, h), true, ImGuiWindowFlags_NoScrollbar);
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

            // Any field edit invalidates a stale Test-connection result.
            auto clearStaleTestResult = [&]() {
                if (!d.assistantPrefsTestInFlight) {
                    d.assistantPrefsTestResult.clear();
                    d.assistantPrefsTestResultType = 0;
                }
            };

            // Async probe helper — runs ProbeReachability on a worker thread and
            // posts the result back via MainThreadDispatcher. Captured by value into
            // the worker lambda so a concurrent UI edit during the probe doesn't
            // change the bytes the worker sees. Cancel-on-close (above) flips the
            // cancel atom so the posted callback short-circuits if Preferences
            // closes mid-probe.
            auto runProbe = [&app, &d](TrackerConfig probeCfg, AiProvider provider) {
                LOG_INFO("Preferences: Test connection start providerKind=%d", static_cast<int>(provider));
                d.assistantPrefsTestInFlight = true;
                d.assistantPrefsTestResult = "Testing...";
                d.assistantPrefsTestResultType = 0;
                d.assistantPrefsTestCancel = std::make_shared<std::atomic<bool>>(false);
                auto cancel = d.assistantPrefsTestCancel;
                // Provider-aware ApiKey / BaseUrl / ModelId pick. Mirrors
                // `BuildClientConfig` + `ResolveModelId` in `AiAssistantController.cpp`.
                std::string apiKey;
                std::string baseUrl;
                std::string modelId;
                switch (provider) {
                case AiProvider::Anthropic:
                    apiKey = probeCfg.AiAnthropicApiKey;
                    baseUrl = probeCfg.AiBaseUrl;
                    modelId = probeCfg.AiModelAnthropic;
                    break;
                case AiProvider::OllamaNative:
                    apiKey.clear();
                    baseUrl = probeCfg.AiOllamaBaseUrl;
                    modelId = probeCfg.AiModelOllama;
                    break;
                case AiProvider::OllamaOpenAiCompat:
                    apiKey = probeCfg.AiApiKey;
                    baseUrl = probeCfg.AiBaseUrl.empty() ? probeCfg.AiOllamaBaseUrl : probeCfg.AiBaseUrl;
                    modelId = probeCfg.AiModelOpenAi;
                    break;
                case AiProvider::OpenAi:
                default:
                    apiKey = probeCfg.AiApiKey;
                    baseUrl = probeCfg.AiBaseUrl;
                    modelId = probeCfg.AiModelOpenAi;
                    break;
                }
                // When the configured base URL is empty for a local provider, fall back
                // to the canonical default so the user can click Test connection right
                // after picking the provider — no manual URL entry needed. The default
                // is also recorded so the success callback can persist it back into cfg
                // (and the buffer reseed reflects it in the field).
                std::string defaultedBaseUrl;
                if (baseUrl.empty()) {
                    if (provider == AiProvider::OllamaOpenAiCompat) {
                        defaultedBaseUrl = "http://127.0.0.1:1234";
                    } else if (provider == AiProvider::OllamaNative) {
                        defaultedBaseUrl = "http://localhost:11434";
                    }
                    baseUrl = defaultedBaseUrl;
                }
                // Strip header-smuggling control characters from the key (cheap
                // defence-in-depth; libcurl rejects them too).
                std::string sanitisedKey;
                sanitisedKey.reserve(apiKey.size());
                std::copy_if(apiKey.begin(), apiKey.end(), std::back_inserter(sanitisedKey),
                             [](char c) { return c != '\r' && c != '\n' && c != '\0'; });
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
                clientCfg.TotalTimeoutMs = 15000;

                MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;
                std::thread([provider, clientCfg, cancel, defaultedBaseUrl, modelId, &dispatcher]() {
                    std::string errMsg;
                    // Defensive try/catch — `MakeAiClient` / `ProbeReachability` /
                    // `SendStreaming` all run third-party transport (cpr/libcurl) +
                    // SSE parser code. An uncaught exception here would propagate
                    // out of the detached thread and call `std::terminate`. Trap
                    // it, surface as a failure result via the existing dispatcher
                    // path so UI state (in-flight flag + result line) recovers.
                    try {
                        std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
                        if (!client) {
                            errMsg = "Provider not available in this build.";
                        } else {
                            // Step 1: reachability — server alive + auth accepted on the
                            // listing endpoint (cheap GET).
                            errMsg = client->ProbeReachability(clientCfg);
                            // Step 2: real chat handshake — sends a 1-token "ping" against
                            // the configured model so model-not-found / chat-disabled /
                            // missing-loaded-model errors surface BEFORE the user types
                            // their first real prompt. This is what made earlier Test-
                            // connection passes mislead users into thinking the full chat
                            // path worked (it didn't — /v1/models OK ≠ /v1/chat/completions
                            // OK against a loaded model).
                            if (errMsg.empty()) {
                                if (modelId.empty()) {
                                    errMsg = "chat: model id is empty (set 'Model' field)";
                                } else {
                                    AiChatRequest req;
                                    req.Model = modelId;
                                    AiMessage userMsg;
                                    userMsg.Role = "user";
                                    userMsg.Content = "ping";
                                    req.History.push_back(std::move(userMsg));
                                    req.MaxTokens = 4;
                                    std::atomic<bool> sawDelta(false);
                                    std::string chatErr;
                                    auto onDelta = [&](const AiStreamDelta& d2) {
                                        if (!d2.TokenChunk.empty() || d2.IsFinal) {
                                            sawDelta.store(true);
                                        }
                                    };
                                    auto onError = [&](const AiStreamError& e) { chatErr = e.Message; };
                                    client->SendStreaming(clientCfg, req, onDelta, onError, cancel);
                                    if (!chatErr.empty()) {
                                        errMsg = std::string("chat: ") + chatErr;
                                    } else if (!sawDelta.load()) {
                                        errMsg = "chat: server returned no content";
                                    }
                                }
                            }
                        }
                    } catch (const std::exception& ex) {
                        errMsg = std::string("internal error: ") + ex.what();
                    } catch (...) {
                        errMsg = "internal error: unknown exception";
                    }
                    dispatcher.PostToMainThread([errMsg, cancel, provider, defaultedBaseUrl]() {
                        if (cancel && cancel->load()) {
                            return;
                        }
                        g_ui.assistantPrefsTestInFlight = false;
                        if (errMsg.empty()) {
                            LOG_INFO("Preferences: Test connection VERIFIED providerKind=%d defaultedBaseUrl='%s'",
                                     static_cast<int>(provider), defaultedBaseUrl.c_str());
                            g_ui.assistantPrefsTestResult = "Verified.";
                            g_ui.assistantPrefsTestResultType = 1;
                            // On success with a defaulted URL, persist the default into
                            // cfg + force a buffer reseed so the field shows the value
                            // the probe used.
                            if (!defaultedBaseUrl.empty()) {
                                if (provider == AiProvider::OllamaOpenAiCompat) {
                                    g_ui.cfg.AiBaseUrl = defaultedBaseUrl;
                                } else if (provider == AiProvider::OllamaNative) {
                                    g_ui.cfg.AiOllamaBaseUrl = defaultedBaseUrl;
                                }
                                MarkPrefsDirty(g_ui);
                                g_ui.assistantPrefsForceBufferReseed = true;
                            }
                        } else {
                            LOG_ERROR("Preferences: Test connection FAILED providerKind=%d errMsg='%s'",
                                      static_cast<int>(provider), errMsg.c_str());
                            g_ui.assistantPrefsTestResult = std::string("Failed: ") + errMsg;
                            g_ui.assistantPrefsTestResultType = 2;
                        }
                    });
                }).detach();
            };

            // --- Provider Combo (top). Picking from this dropdown seeds sensible
            // defaults for the chosen provider so the LM Studio / Ollama happy paths
            // need zero extra fields.
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
                const int newKind = static_cast<int>(providers[providerIdx].Kind);
                LOG_INFO("Preferences: AiProviderKind %d -> %d", d.cfg.AiProviderKind, newKind);
                d.cfg.AiProviderKind = newKind;
                MarkPrefsDirty(d);
                clearStaleTestResult();
            }
            const AiProvider selectedKind = providers[providerIdx].Kind;

            // --- Test connection button (top, prominent, always enabled). When the
            // configured state can't possibly succeed the probe itself reports a
            // clear server-side error — better UX than disabling the button.
            ImGui::Spacing();
            const bool testInFlight = d.assistantPrefsTestInFlight;
            if (testInFlight) {
                ImGui::BeginDisabled(true);
            }
            const bool testPressed = ImGui::Button("Test connection");
            if (testInFlight) {
                ImGui::EndDisabled();
            }
            if (testPressed && !testInFlight) {
                runProbe(d.cfg, selectedKind);
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
            } else {
                ImGui::TextDisabled("Click to verify the configured provider can be reached.");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Per-provider credentials (auto-saved on every edit). ---
            //
            // Model picker shape: when the provider ships a published catalog
            // (`KnownModels(provider)` non-empty), render a Combo + a collapsing
            // "Custom model ID" header for free-form override. Otherwise (local
            // OpenAI-compat / Ollama-native, models are user-side) render plain
            // InputText with a hint.
            auto renderModelPicker = [&](const char* comboLabel, const char* freeFormLabel, const char* freeFormHint,
                                         AiProvider catalogProvider, char* modelBuf, std::size_t modelBufCap,
                                         std::string& cfgField) {
                const std::vector<smatchet::ai::ModelOption> catalog = smatchet::ai::KnownModels(catalogProvider);
                if (catalog.empty()) {
                    if (ImGui::InputTextWithHint(freeFormLabel, freeFormHint, modelBuf,
                                                 static_cast<int>(modelBufCap))) {
                        cfgField = modelBuf;
                        MarkPrefsDirty(d);
                        clearStaleTestResult();
                    }
                    return;
                }
                std::vector<const char*> displayPtrs;
                displayPtrs.reserve(catalog.size());
                std::transform(catalog.begin(), catalog.end(), std::back_inserter(displayPtrs),
                               [](const smatchet::ai::ModelOption& m) { return m.DisplayName.c_str(); });
                int selectedIdx = -1;
                auto it = std::find_if(catalog.begin(), catalog.end(),
                                       [&](const smatchet::ai::ModelOption& m) { return m.Id == modelBuf; });
                if (it != catalog.end()) {
                    selectedIdx = static_cast<int>(std::distance(catalog.begin(), it));
                }
                int comboIdx = (selectedIdx >= 0) ? selectedIdx : 0;
                if (ImGui::Combo(comboLabel, &comboIdx, displayPtrs.data(), static_cast<int>(displayPtrs.size()))) {
                    std::snprintf(modelBuf, modelBufCap, "%s", catalog[static_cast<std::size_t>(comboIdx)].Id.c_str());
                    cfgField = modelBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                if (selectedIdx < 0 && modelBuf[0] != '\0') {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(custom: %s)", modelBuf);
                }
                if (ImGui::CollapsingHeader("Custom model ID (advanced)")) {
                    if (ImGui::InputText("##model_custom", modelBuf, static_cast<int>(modelBufCap))) {
                        cfgField = modelBuf;
                        MarkPrefsDirty(d);
                        clearStaleTestResult();
                    }
                }
            };

            if (selectedKind == AiProvider::OpenAi || selectedKind == AiProvider::OllamaOpenAiCompat) {
                const bool isLocalCompat = (selectedKind == AiProvider::OllamaOpenAiCompat);
                const char* keyLabel = isLocalCompat ? "API key (optional for local)" : "OpenAI API key";
                if (ImGui::InputText(keyLabel, s_openAiKeyBuf, sizeof(s_openAiKeyBuf), ImGuiInputTextFlags_Password)) {
                    d.cfg.AiApiKey = s_openAiKeyBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                // OllamaOpenAiCompat keeps an empty catalog — the local server
                // names its own models. `renderModelPicker` falls back to free-form
                // hint in that case. OpenAi has a catalog so the Combo renders.
                const char* modelComboLabel = isLocalCompat ? "Model" : "OpenAI model";
                const char* modelFreeFormLabel = modelComboLabel;
                const char* modelHint = isLocalCompat ? "e.g. local-model, llama3, qwen2.5" : "";
                renderModelPicker(modelComboLabel, modelFreeFormLabel, modelHint, selectedKind, s_openAiModelBuf,
                                  sizeof(s_openAiModelBuf), d.cfg.AiModelOpenAi);
                const char* urlHint = isLocalCompat ? "http://127.0.0.1:1234 (LM Studio)" : "https://api.openai.com";
                if (ImGui::InputTextWithHint("Base URL", urlHint, s_baseUrlBuf, sizeof(s_baseUrlBuf))) {
                    d.cfg.AiBaseUrl = s_baseUrlBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
            } else if (selectedKind == AiProvider::Anthropic) {
                if (ImGui::InputText("Anthropic API key", s_anthropicKeyBuf, sizeof(s_anthropicKeyBuf),
                                     ImGuiInputTextFlags_Password)) {
                    d.cfg.AiAnthropicApiKey = s_anthropicKeyBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                renderModelPicker("Anthropic model", "Anthropic model", "", AiProvider::Anthropic, s_anthropicModelBuf,
                                  sizeof(s_anthropicModelBuf), d.cfg.AiModelAnthropic);
            } else if (selectedKind == AiProvider::OllamaNative) {
                renderModelPicker("Ollama model", "Ollama model", "e.g. llama3, qwen2.5, mistral",
                                  AiProvider::OllamaNative, s_ollamaModelBuf, sizeof(s_ollamaModelBuf),
                                  d.cfg.AiModelOllama);
                if (ImGui::InputTextWithHint("Ollama base URL", "http://localhost:11434", s_ollamaBaseUrlBuf,
                                             sizeof(s_ollamaBaseUrlBuf))) {
                    d.cfg.AiOllamaBaseUrl = s_ollamaBaseUrlBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
            }

            // --- Default reasoning effort (all providers) ---
            // Stored as a string enum: "auto" | "low" | "medium" | "high". "auto"
            // omits the wire parameter (server picks). Forwarded as the
            // `reasoning_effort` body field by OpenAiClient; providers that
            // don't understand it ignore it. The chat-window header has a per-
            // turn Combo that overrides this default for one Send.
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                const char* kEffortLabels[] = {"Auto (server picks)", "Low", "Medium", "High"};
                const char* kEffortIds[] = {"auto", "low", "medium", "high"};
                int effortIdx = 0;
                for (int i = 0; i < 4; ++i) {
                    if (d.cfg.AiReasoningEffort == kEffortIds[i]) {
                        effortIdx = i;
                        break;
                    }
                }
                if (ImGui::Combo("Default reasoning effort", &effortIdx, kEffortLabels, 4)) {
                    LOG_INFO("Preferences: AiReasoningEffort %s -> %s", d.cfg.AiReasoningEffort.c_str(),
                             kEffortIds[effortIdx]);
                    d.cfg.AiReasoningEffort = kEffortIds[effortIdx];
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                ImGui::SetItemTooltip("OpenAI `reasoning_effort` body parameter for o-series / reasoning-tuned "
                                      "models. LM Studio + LocalAI pass it through to local reasoning models "
                                      "(Qwen3, gemma-3, etc.). Providers that don't understand the parameter "
                                      "ignore it.");
            }

            // --- agents.md harness (optional) ---
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("agents.md harness (optional)");
            ImGui::TextWrapped("Layered system prompt injected into every Assistant turn. Global layer defaults to "
                               "%%LOCALAPPDATA%%/Smatchet/agents.md when blank. Each layer capped at 64 KB.");
            ImGui::Spacing();
            if (ImGui::InputText("Global agents.md path", s_agentsMdGlobalBuf, sizeof(s_agentsMdGlobalBuf))) {
                d.cfg.AgentsMdGlobalPath = s_agentsMdGlobalBuf;
                MarkPrefsDirty(d);
                if (app.HasAiAssistantController()) {
                    app.GetAiAssistantController().InvalidateAgentsMdCache();
                }
            }
            ImGui::SetItemTooltip("Default %LOCALAPPDATA%/Smatchet/agents.md when blank. Override to point at a "
                                  "checked-in shared file.");
            if (ImGui::InputText("Project agents.md path (override)", s_projectAgentsMdBuf,
                                 sizeof(s_projectAgentsMdBuf))) {
                d.cfg.ProjectAgentsMdPath = s_projectAgentsMdBuf;
                MarkPrefsDirty(d);
                if (app.HasAiAssistantController()) {
                    app.GetAiAssistantController().InvalidateAgentsMdCache();
                }
            }
            ImGui::SetItemTooltip("When set, this exact path is used as the project layer. Leave blank to disable the "
                                  "project layer entirely unless Auto-discover is enabled below.");
            bool autoDiscover = d.cfg.AgentsMdAutoDiscoverProject;
            if (ImGui::Checkbox("Auto-discover project agents.md (walk up from cwd)", &autoDiscover)) {
                d.cfg.AgentsMdAutoDiscoverProject = autoDiscover;
                MarkPrefsDirty(d);
                if (app.HasAiAssistantController()) {
                    app.GetAiAssistantController().InvalidateAgentsMdCache();
                }
            }
            ImGui::SetItemTooltip("OFF (default): only the Global file + explicit Project path are used. ON: walks up "
                                  "the cwd chain looking for agents.md / AGENTS.md.");

            ImGui::EndTabItem();
        }
#endif
#if defined(SMATCHET_WITH_AGENTIC)
        // T7 — Agentic flow Preferences tab. Master toggle + interval + source + query +
        // GitHub PAT (DPAPI-encrypted via T1, same code path as cfg.GitHubPat).
        // `RestartAgenticPoll()` fires on master-toggle flip so the worker thread picks up
        // the new state without an app restart; field edits debounce-save via MarkPrefsDirty.
        if (ImGui::BeginTabItem(SmatchetLocalization::T("agent.prefs.tabTitle", "Agentic"))) {
            ImGui::TextWrapped("Scheduled agentic triage. When enabled, Smatchet polls the configured "
                               "repository at the chosen interval, runs each updated issue through the LLM, "
                               "and persists draft actions to the Proposals panel for your review. All API "
                               "writes wait on your approval — nothing is applied without explicit consent.");
            ImGui::Spacing();

            if (ImGui::Checkbox(SmatchetLocalization::T("agent.prefs.enableToggle", "Enable scheduled agentic triage"),
                                &d.cfg.AgenticPollEnabled)) {
                MarkPrefsDirty(d);
                // (Bundle A) Flip applies immediately. `RestartAgenticPollAsync` signals
                // the worker to stop + defers the join to the next dispatcher drain so
                // the UI never blocks on a mid-batch LLM call (was: synchronous join
                // froze the UI for multi-minute windows under Pillar 2). The new worker
                // starts immediately; the old one drains off-band.
                app.RestartAgenticPollAsync();
            }

            // Interval (clamped 60..3600).
            ImGui::TextUnformatted(SmatchetLocalization::T("agent.prefs.intervalLabel", "Interval:"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputInt("##AgenticPollInterval", &d.cfg.AgenticPollIntervalSec, 30, 60)) {
                if (d.cfg.AgenticPollIntervalSec < 60) {
                    d.cfg.AgenticPollIntervalSec = 60;
                } else if (d.cfg.AgenticPollIntervalSec > 3600) {
                    d.cfg.AgenticPollIntervalSec = 3600;
                }
                MarkPrefsDirty(d);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(SmatchetLocalization::T("agent.prefs.intervalUnit", "seconds (60..3600)"));

            // Source combo — only "github" today; greyed dropdown keeps the surface visible
            // for the next-backend extension (Plane / Linear / Jira-agentic) without churn.
            ImGui::TextUnformatted(SmatchetLocalization::T("agent.prefs.sourceLabel", "Source:"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            int sourceIdx = 0;
            const char* sourceLabels[1] = {"github"};
            ImGui::BeginDisabled(true);
            ImGui::Combo("##AgenticPollSource", &sourceIdx, sourceLabels, 1);
            ImGui::EndDisabled();
            // Round-trip the source value to whatever the user chose (only "github" today —
            // a no-op write per loop, but keeps the contract simple).
            d.cfg.AgenticPollSource = "github";

            // Query — `OWNER/REPO` for source=github. Persists via MarkPrefsDirty.
            ImGui::TextUnformatted(SmatchetLocalization::T("agent.prefs.queryLabel", "Query:"));
            ImGui::SameLine();
            char queryBuf[256];
            CopyStringToBuffer(queryBuf, d.cfg.AgenticPollQuery);
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::InputText("##AgenticPollQuery", queryBuf, sizeof(queryBuf))) {
                d.cfg.AgenticPollQuery = queryBuf;
                MarkPrefsDirty(d);
            }
            ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.prefs.queryHint.github",
                                                              "For github: OWNER/REPO of the repository to poll"));

            // GitHub PAT — passworded. Persists via MarkPrefsDirty (DPAPI-encrypted on Save
            // through the same path as the existing Assistant tab's API key fields).
            ImGui::TextUnformatted(SmatchetLocalization::T("agent.prefs.githubPatLabel", "GitHub PAT:"));
            ImGui::SameLine();
            char patBuf[512];
            CopyStringToBuffer(patBuf, d.cfg.GitHubPat);
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::InputText("##AgenticGitHubPat", patBuf, sizeof(patBuf), ImGuiInputTextFlags_Password)) {
                d.cfg.GitHubPat = patBuf;
                MarkPrefsDirty(d);
            }
            ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.prefs.githubPatHint",
                                                              "Bearer token - needs `repo` + `issues` scope"));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Last-poll / Next-poll readout. Worker stamps `agenticPollLastAtSec_` at the end
            // of every iteration; 0 means "never this session".
            const std::int64_t lastPollSec = app.GetAgenticLastPollAtSec();
            ImGui::TextUnformatted(SmatchetLocalization::T("agent.prefs.lastPoll", "Last poll:"));
            ImGui::SameLine();
            if (lastPollSec == 0) {
                ImGui::TextUnformatted(SmatchetLocalization::T("agent.prefs.lastPollNever", "never"));
            } else {
                const std::int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count();
                const std::int64_t agoSec = (nowSec > lastPollSec) ? (nowSec - lastPollSec) : 0;
                const long long mins = static_cast<long long>(agoSec / 60);
                const long long secs = static_cast<long long>(agoSec % 60);
                ImGui::Text("%llds ago (%lldm %llds)", static_cast<long long>(agoSec), mins, secs);
                if (d.cfg.AgenticPollEnabled) {
                    const std::int64_t nextSec = lastPollSec + d.cfg.AgenticPollIntervalSec;
                    const std::int64_t deltaSec = (nextSec > nowSec) ? (nextSec - nowSec) : 0;
                    const long long nm = static_cast<long long>(deltaSec / 60);
                    const long long ns = static_cast<long long>(deltaSec % 60);
                    char timeStr[64];
                    std::snprintf(timeStr, sizeof(timeStr), "%lldm %llds", nm, ns);
                    char nextStr[160];
                    std::snprintf(nextStr, sizeof(nextStr), "%s",
                                  SmatchetLocalization::T("agent.prefs.nextPoll", "Next poll: ~in {0}"));
                    // Manual substitution since the existing localization helpers don't ship
                    // a positional formatter. Worst case: a partial render if `{0}` is missing.
                    std::string nextDisplay = nextStr;
                    const std::size_t slot = nextDisplay.find("{0}");
                    if (slot != std::string::npos) {
                        nextDisplay.replace(slot, 3, timeStr);
                    } else {
                        nextDisplay += " ";
                        nextDisplay += timeStr;
                    }
                    ImGui::TextUnformatted(nextDisplay.c_str());
                }
            }

            ImGui::Spacing();

            // Run-now button — synchronous on the UI thread would freeze for ~5-30 seconds
            // (LLM round-trip per issue). Per AGENTS.md Pillar 2 we wrap in
            // LaunchBackgroundTask; results land in SQLite and the T6 panel picks them up.
            if (ImGui::Button(SmatchetLocalization::T("agent.prefs.runNow", "Run triage now"))) {
                app.LaunchBackgroundTask([&app]() {
                    std::string runErr;
                    if (!app.RunAgenticTriageOnce(runErr)) {
                        LOG_WARN("Preferences: Run triage now failed: %s", runErr.c_str());
                    }
                });
            }
            ImGui::EndTabItem();
        }
#endif // SMATCHET_WITH_AGENTIC
#if defined(SMATCHET_WITH_WHISPER)
        // Whisper dictation Preferences tab — Phase C. Hotkey rebind UI lands
        // in Phase E (read-only display here). Master toggle persists through
        // MarkPrefsDirty (debounced save). Download / cancel buttons share the
        // banner-owned ModelDownloader so a fetch started from the banner
        // continues to show progress on this tab.
        if (ImGui::BeginTabItem(SmatchetLocalization::T("whisper.preferences.tabTitle", "Whisper"))) {
            ImGui::TextWrapped("Push-to-talk dictation. Hold the configured hotkey, speak, release. Transcription "
                               "runs locally when a Whisper model is on disk; falls back to OpenAI Whisper API "
                               "when no model is present (cloud mode requires an API key).");
            ImGui::Spacing();

            if (ImGui::Checkbox(SmatchetLocalization::T("whisper.preferences.enableToggle", "Enable voice dictation"),
                                &d.cfg.WhisperEnabled)) {
                if (d.cfg.WhisperEnabled) {
                    d.cfg.WhisperSetupCompleted = true;
                    d.cfg.WhisperSetupChoice = "enabled";
                } else {
                    d.cfg.WhisperSetupChoice = "disabled";
                }
                MarkPrefsDirty(d);
            }

            // Mode selector.
            {
                int modeIdx = 0;
                if (d.cfg.WhisperMode == "local") {
                    modeIdx = 1;
                } else if (d.cfg.WhisperMode == "cloud") {
                    modeIdx = 2;
                }
                const char* labels[3] = {
                    SmatchetLocalization::T("whisper.preferences.modeAuto", "Auto (local if present, cloud fallback)"),
                    SmatchetLocalization::T("whisper.preferences.modeLocal", "Local only (no network)"),
                    SmatchetLocalization::T("whisper.preferences.modeCloud", "Cloud only (OpenAI)")};
                if (ImGui::Combo("Mode", &modeIdx, labels, 3)) {
                    d.cfg.WhisperMode = (modeIdx == 1) ? "local" : (modeIdx == 2) ? "cloud" : "auto";
                    MarkPrefsDirty(d);
                }
            }

            // Model picker + download button.
            const std::string sharedDir = ConfigManager::GetPlatformSharedUserDataDirectory();
            std::string modelDir;
            if (!sharedDir.empty()) {
                modelDir = sharedDir;
                if (!modelDir.empty() && modelDir.back() != '/' && modelDir.back() != '\\') {
                    modelDir.push_back('/');
                }
                modelDir += "whisper";
            }
            const auto& catalog = smatchet::whisper::catalog::All();
            int selIdx = 1; // default Recommended
            for (std::size_t i = 0; i < catalog.size(); ++i) {
                if (catalog[i].Id == d.cfg.WhisperModel) {
                    selIdx = static_cast<int>(i);
                    break;
                }
            }
            std::vector<std::string> labelStorage;
            std::vector<const char*> labelPtrs;
            labelStorage.reserve(catalog.size());
            for (std::size_t i = 0; i < catalog.size(); ++i) {
                std::string lbl = catalog[i].DisplayName + " (" + catalog[i].Id + ")";
                if (smatchet::whisper::catalog::IsModelPresent(catalog[i].Id, modelDir)) {
                    lbl += " ";
                    lbl += SmatchetLocalization::T("whisper.preferences.modelPresent", "(installed)");
                }
                labelStorage.push_back(std::move(lbl));
            }
            labelPtrs.reserve(labelStorage.size());
            for (const auto& s : labelStorage) {
                labelPtrs.push_back(s.c_str());
            }
            if (ImGui::Combo(SmatchetLocalization::T("whisper.preferences.model", "Speech model"), &selIdx,
                             labelPtrs.data(), static_cast<int>(labelPtrs.size()))) {
                if (selIdx >= 0 && static_cast<std::size_t>(selIdx) < catalog.size()) {
                    d.cfg.WhisperModel = catalog[static_cast<std::size_t>(selIdx)].Id;
                    MarkPrefsDirty(d);
                }
            }

            smatchet::whisper::ModelDownloader& dl = smatchet::whisper::banner::BannerOwnedDownloader();
            const auto prog = dl.GetProgress();
            const bool currentlyDownloading = (prog.state == smatchet::whisper::ModelDownloader::State::Downloading) ||
                                              (prog.state == smatchet::whisper::ModelDownloader::State::Verifying);
            const bool modelPresent = smatchet::whisper::catalog::IsModelPresent(d.cfg.WhisperModel, modelDir);

            ImGui::BeginDisabled(modelPresent || currentlyDownloading || modelDir.empty());
            if (ImGui::Button(SmatchetLocalization::T("whisper.preferences.downloadModel", "Download model"))) {
                // Stamp fresh consent for the gate before the worker reads it.
                d.cfg.WhisperConsentTimestampSec = smatchet::whisper::consent::NowEpochSec();
                ConfigManager::Save(d.cfg);
                std::string err;
                if (!dl.Start(app, d.cfg.WhisperModel, modelDir, err)) {
                    LOG_WARN("Whisper preferences: download dispatch failed: %s", err.c_str());
                }
            }
            ImGui::EndDisabled();
            if (currentlyDownloading) {
                ImGui::SameLine();
                if (ImGui::Button(SmatchetLocalization::T("whisper.preferences.cancelDownload", "Cancel download"))) {
                    dl.Cancel();
                }
                const float frac = (prog.bytesExpected > 0) ? std::min(1.0f, static_cast<float>(prog.bytesReceived) /
                                                                                 static_cast<float>(prog.bytesExpected))
                                                            : 0.0f;
                ImGui::ProgressBar(frac, ImVec2(-1, 0));
            }
            if (!prog.error.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s", prog.error.c_str());
            }

            // Hotkey display + Phase E rebind. Button label flips to the
            // capturing-prompt while the user is binding a new combo. Cancel
            // is Esc; capture completes on the first non-modifier key press,
            // at which point we snapshot the modifier state, Stringify, and
            // persist. Re-registration of the global hotkey happens on the
            // next plugin OnStop/OnStart cycle (or at app restart) — Phase E
            // does not hot-rebind the live hook to keep the surface tight.
            ImGui::Separator();
            ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.hotkey", "Push-to-talk hotkey"));
            {
                static bool s_capturing = false;
                static std::string s_hotkeyError;
                static char s_hotkeyDisplay[64] = {0};
                static bool s_hotkeyDisplaySeeded = false;
                if (!s_hotkeyDisplaySeeded || std::strcmp(s_hotkeyDisplay, d.cfg.WhisperHotkey.c_str()) != 0) {
                    std::snprintf(s_hotkeyDisplay, sizeof(s_hotkeyDisplay), "%s", d.cfg.WhisperHotkey.c_str());
                    s_hotkeyDisplaySeeded = true;
                }

                if (!s_capturing) {
                    ImGui::TextDisabled("%s", s_hotkeyDisplay);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(
                            SmatchetLocalization::T("whisper.preferences.hotkeyRebindButton", "Click to rebind"))) {
                        s_capturing = true;
                        s_hotkeyError.clear();
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.30f, 1.0f), "%s",
                                       SmatchetLocalization::T("whisper.preferences.hotkeyCapturing",
                                                               "Press a key combo... (Esc to cancel)"));

                    // Esc cancels capture without clobbering the existing key.
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        s_capturing = false;
                        s_hotkeyError.clear();
                    } else {
                        // Walk the small set of supported non-modifier keys
                        // and grab the first one pressed this frame.
                        struct CaptureKey {
                            ImGuiKey imguiKey;
                            unsigned int vk;
                        };
                        const ImGuiIO& io = ImGui::GetIO();
                        unsigned int capturedVk = 0;
                        // Letters A..Z map ImGuiKey_A..ImGuiKey_Z to ASCII VKs.
                        for (int k = 0; k < 26 && capturedVk == 0; ++k) {
                            const ImGuiKey ik = static_cast<ImGuiKey>(ImGuiKey_A + k);
                            if (ImGui::IsKeyPressed(ik, false)) {
                                capturedVk = static_cast<unsigned int>('A' + k);
                            }
                        }
                        // Digits 0..9.
                        for (int k = 0; k < 10 && capturedVk == 0; ++k) {
                            const ImGuiKey ik = static_cast<ImGuiKey>(ImGuiKey_0 + k);
                            if (ImGui::IsKeyPressed(ik, false)) {
                                capturedVk = static_cast<unsigned int>('0' + k);
                            }
                        }
                        // Function keys F1..F12 (ImGui exposes up to F24 in some
                        // builds, but F1..F12 covers the realistic rebind set).
                        for (int k = 0; k < 12 && capturedVk == 0; ++k) {
                            const ImGuiKey ik = static_cast<ImGuiKey>(ImGuiKey_F1 + k);
                            if (ImGui::IsKeyPressed(ik, false)) {
                                capturedVk = smatchet::whisper::hotkey::vk::kF1 + static_cast<unsigned int>(k);
                            }
                        }
                        if (capturedVk == 0 && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                            capturedVk = smatchet::whisper::hotkey::vk::kSpace;
                        }
                        if (capturedVk == 0 && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                            capturedVk = smatchet::whisper::hotkey::vk::kTab;
                        }
                        if (capturedVk == 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                            capturedVk = smatchet::whisper::hotkey::vk::kEnter;
                        }

                        if (capturedVk != 0) {
                            smatchet::whisper::hotkey::Hotkey hk;
                            if (io.KeyCtrl)
                                hk.mods |= smatchet::whisper::hotkey::mod::kControl;
                            if (io.KeyAlt)
                                hk.mods |= smatchet::whisper::hotkey::mod::kAlt;
                            if (io.KeyShift)
                                hk.mods |= smatchet::whisper::hotkey::mod::kShift;
                            if (io.KeySuper)
                                hk.mods |= smatchet::whisper::hotkey::mod::kWin;
                            hk.vk = capturedVk;

                            // Reject combos with no modifier — a bare key is
                            // a global hotkey landmine.
                            if (hk.mods == 0) {
                                s_hotkeyError = SmatchetLocalization::T("whisper.preferences.hotkeyErrorModifiersOnly",
                                                                        "Hotkey must include a non-modifier key");
                                s_capturing = false;
                            } else {
                                const std::string newDescriptor = smatchet::whisper::hotkey::Stringify(hk);
                                if (newDescriptor.empty()) {
                                    s_hotkeyError = SmatchetLocalization::T("whisper.preferences.hotkeyErrorParse",
                                                                            "Could not parse the captured key combo");
                                } else {
                                    d.cfg.WhisperHotkey = newDescriptor;
                                    MarkPrefsDirty(d);
                                    std::snprintf(s_hotkeyDisplay, sizeof(s_hotkeyDisplay), "%s",
                                                  newDescriptor.c_str());
                                    s_hotkeyError.clear();
                                    // Phase F — live hot-rebind: re-register the
                                    // Win32 global hook against the new descriptor
                                    // immediately so users don't need to restart.
                                    // On register-failure surface the error inline
                                    // (the previous hook is already torn down by
                                    // ReregisterHotkey on its way through; user is
                                    // left without push-to-talk until they pick a
                                    // working combo or restart).
                                    WhisperPlugin* plug = WhisperPlugin::InstanceForUi();
                                    if (plug != nullptr) {
                                        std::string rebindErr;
                                        if (!plug->ReregisterHotkey(newDescriptor, rebindErr)) {
                                            s_hotkeyError = std::string(SmatchetLocalization::T(
                                                                "whisper.preferences.hotkeyErrorRebind",
                                                                "Hotkey rebind failed: ")) +
                                                            rebindErr;
                                            LOG_WARN("Whisper preferences: hot rebind failed: %s", rebindErr.c_str());
                                        } else {
                                            LOG_INFO("Whisper preferences: hot rebind applied to '%s'",
                                                     newDescriptor.c_str());
                                        }
                                    }
                                }
                                s_capturing = false;
                            }
                        }
                    }
                }
                if (!s_hotkeyError.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s", s_hotkeyError.c_str());
                }
            }

            // API key — passworded input, with fallback hint when empty + AiProvider=openai.
            ImGui::Separator();
            ImGui::TextUnformatted(
                SmatchetLocalization::T("whisper.preferences.apiKey", "OpenAI API key (cloud mode)"));
            static char s_whisperKeyBuf[512] = {0};
            static bool s_whisperKeyBufSeeded = false;
            if (!s_whisperKeyBufSeeded) {
                std::snprintf(s_whisperKeyBuf, sizeof(s_whisperKeyBuf), "%s", d.cfg.WhisperApiKey.c_str());
                s_whisperKeyBufSeeded = true;
            }
            if (ImGui::InputText("##WhisperApiKey", s_whisperKeyBuf, sizeof(s_whisperKeyBuf),
                                 ImGuiInputTextFlags_Password)) {
                d.cfg.WhisperApiKey = s_whisperKeyBuf;
                MarkPrefsDirty(d);
            }
            if (d.cfg.WhisperApiKey.empty() && d.cfg.AiProviderKind == 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", SmatchetLocalization::T("whisper.preferences.apiKeyFallback",
                                                                  "(uses AI Assistant OpenAI key when empty)"));
            }

            // Phase F — Test connection button. Dispatches a tiny worker that
            // hits OpenAI's /v1/models with the resolved key (via
            // WhisperApiKeyResolve::Resolve fallback rule) and reports
            // success/failure inline. Mirrors the AI Assistant tab's pattern
            // (LaunchBackgroundTask + MainThreadDispatcher post-back); the
            // status string lives in two file-scope statics so the result
            // survives the user navigating other tabs while the probe runs.
            {
                static std::atomic<bool> s_whisperTestInFlight{false};
                static std::string s_whisperTestResult; // empty | success | failure
                static int s_whisperTestResultType = 0; // 0=neutral, 1=ok, 2=fail
                const bool inFlight = s_whisperTestInFlight.load(std::memory_order_acquire);
                if (inFlight) {
                    ImGui::BeginDisabled(true);
                }
                if (ImGui::Button(
                        SmatchetLocalization::T("whisper.preferences.apiKey.testButton", "Test connection"))) {
                    // Resolve the key with the same 5-row fallback the runtime
                    // uses (WhisperApiKey > AiApiKey when provider=openai).
                    const std::string providerStr =
                        (d.cfg.AiProviderKind == 0) ? std::string("openai") : std::string("anthropic");
                    const std::string resolvedKey =
                        smatchet::whisper::pure::ResolveWhisperApiKey(d.cfg.WhisperApiKey, providerStr, d.cfg.AiApiKey);
                    if (resolvedKey.empty()) {
                        s_whisperTestResult =
                            std::string(SmatchetLocalization::T("whisper.preferences.testConnection.failure", "x ")) +
                            "no API key configured";
                        s_whisperTestResultType = 2;
                    } else {
                        s_whisperTestInFlight.store(true, std::memory_order_release);
                        s_whisperTestResult = "Testing...";
                        s_whisperTestResultType = 0;
                        MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;
                        // Worker — minimal GET to /v1/models. cpr is already
                        // linked; reuse WhisperApiClient's transport idioms
                        // (5 s connect, 15 s total) so a hung DNS doesn't
                        // freeze the result for a minute.
                        std::thread([resolvedKey, &dispatcher]() {
                            std::string okMsg;
                            std::string errMsg;
                            try {
                                cpr::Response r =
                                    cpr::Get(cpr::Url{"https://api.openai.com/v1/models"},
                                             cpr::Header{{"Authorization", std::string("Bearer ") + resolvedKey}},
                                             cpr::ConnectTimeout{5000}, cpr::Timeout{15000});
                                if (r.error.code != cpr::ErrorCode::OK) {
                                    errMsg = std::string("transport: ") + r.error.message;
                                } else if (r.status_code < 200 || r.status_code >= 300) {
                                    errMsg = std::string("HTTP ") + std::to_string(r.status_code);
                                } else {
                                    okMsg = "Connected.";
                                }
                            } catch (const std::exception& ex) {
                                errMsg = std::string("internal: ") + ex.what();
                            } catch (...) {
                                errMsg = "internal: unknown exception";
                            }
                            dispatcher.PostToMainThread([okMsg, errMsg]() {
                                s_whisperTestInFlight.store(false, std::memory_order_release);
                                if (errMsg.empty()) {
                                    s_whisperTestResult =
                                        std::string(SmatchetLocalization::T(
                                            "whisper.preferences.testConnection.success", "Connected")) +
                                        " (" + okMsg + ")";
                                    s_whisperTestResultType = 1;
                                } else {
                                    s_whisperTestResult =
                                        std::string(SmatchetLocalization::T(
                                            "whisper.preferences.testConnection.failure", "Failed: ")) +
                                        errMsg;
                                    s_whisperTestResultType = 2;
                                }
                            });
                        }).detach();
                    }
                }
                if (inFlight) {
                    ImGui::EndDisabled();
                }
                if (!s_whisperTestResult.empty()) {
                    ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
                    if (s_whisperTestResultType == 1) {
                        col = ImVec4(0.35f, 0.90f, 0.45f, 1.0f);
                    } else if (s_whisperTestResultType == 2) {
                        col = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(col, "%s", s_whisperTestResult.c_str());
                }
            }

            // --- Test microphone end-to-end (capture-only). Captures 3 s of
            // audio via WindowsAudioCapture, reports total samples + peak
            // amplitude inline so the user can confirm the mic + format
            // path is producing real samples BEFORE saving + relying on it
            // for transcription. No HTTP, no transcription — pure capture
            // smoke. Surfaces silent-format regressions
            // (WAVE_FORMAT_EXTENSIBLE unwrap, EAX-suppressed mic, muted OS
            // capture, etc.) without burning a Whisper API call.
            {
                static std::atomic<bool> s_micTestInFlight{false};
                static std::string s_micTestResult;
                static int s_micTestResultType = 0; // 0=neutral, 1=ok, 2=fail
                const bool inFlight = s_micTestInFlight.load(std::memory_order_acquire);
                if (inFlight) {
                    ImGui::BeginDisabled(true);
                }
                const char* micSpinnerGlyph = "|";
                if (inFlight) {
                    const int slot = static_cast<int>(::ImGui::GetTime() * 8.0) & 3;
                    micSpinnerGlyph = (slot == 0)   ? "|"
                                      : (slot == 1) ? "/"
                                      : (slot == 2) ? "-"
                                                    : "\\";
                }
                char micLabel[128];
                std::snprintf(micLabel, sizeof(micLabel),
                              inFlight ? "%s  %s" : "%s",
                              SmatchetLocalization::T("whisper.preferences.testMic.button",
                                                      "Test microphone (3 s)"),
                              micSpinnerGlyph);
                if (ImGui::Button(micLabel)) {
                    s_micTestInFlight.store(true, std::memory_order_release);
                    s_micTestResult = "Capturing...";
                    s_micTestResultType = 0;
                    MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;
                    std::thread([&dispatcher]() {
                        smatchet::whisper::WindowsAudioCapture cap;
                        std::string startErr;
                        if (!cap.Start(startErr)) {
                            const std::string err = startErr.empty()
                                                       ? std::string("capture start failed")
                                                       : startErr;
                            dispatcher.PostToMainThread([err]() {
                                s_micTestInFlight.store(false, std::memory_order_release);
                                s_micTestResult = std::string("Failed: ") + err;
                                s_micTestResultType = 2;
                            });
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                        cap.Stop();
                        std::vector<std::int16_t> pcm;
                        cap.DrainCapturedPcm(pcm);
                        std::int32_t peakAbs = 0;
                        for (std::int16_t s : pcm) {
                            const std::int32_t m =
                                s >= 0 ? static_cast<std::int32_t>(s)
                                       : -static_cast<std::int32_t>(s);
                            if (m > peakAbs) {
                                peakAbs = m;
                            }
                        }
                        const float peakNorm = static_cast<float>(peakAbs) / 32768.0f;
                        const std::size_t samples = pcm.size();
                        dispatcher.PostToMainThread([samples, peakAbs, peakNorm]() {
                            s_micTestInFlight.store(false, std::memory_order_release);
                            char buf[256];
                            if (samples == 0) {
                                std::snprintf(buf, sizeof(buf),
                                              "Failed: 0 samples captured (mic unplugged / muted / "
                                              "consent denied)");
                                s_micTestResultType = 2;
                            } else if (peakAbs == 0) {
                                std::snprintf(
                                    buf, sizeof(buf),
                                    "Failed: %zu samples but peak=0 (format unwrap broken or mic "
                                    "muted at hardware)",
                                    samples);
                                s_micTestResultType = 2;
                            } else if (peakNorm < 0.01f) {
                                std::snprintf(buf, sizeof(buf),
                                              "Warn: %zu samples, peak=%.3f (very quiet — speak "
                                              "louder or check mic gain)",
                                              samples, peakNorm);
                                s_micTestResultType = 2;
                            } else {
                                std::snprintf(buf, sizeof(buf),
                                              "OK: %zu samples, peak=%.3f (ready for "
                                              "transcription)",
                                              samples, peakNorm);
                                s_micTestResultType = 1;
                            }
                            s_micTestResult = buf;
                        });
                    }).detach();
                }
                if (inFlight) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(captures 3 s; reports sample count + peak)");
                if (!s_micTestResult.empty()) {
                    ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
                    if (s_micTestResultType == 1) col = ImVec4(0.35f, 0.90f, 0.45f, 1.0f);
                    if (s_micTestResultType == 2) col = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
                    ImGui::TextColored(col, "%s", s_micTestResult.c_str());
                }
            }

            // --- Test transcription end-to-end. Captures 4 s, runs the
            // full transcription pipeline (silence trim + mode router +
            // cloud / local) and surfaces the resulting text inline. The
            // first end-to-end "does this actually work" button — bridges
            // the gap between "Test connection" (HTTP key probe only) and
            // "hold hotkey + speak" (no feedback if it failed silently).
            {
                static std::atomic<bool> s_e2eInFlight{false};
                static std::string s_e2eResult;
                static int s_e2eResultType = 0;
                const bool inFlight = s_e2eInFlight.load(std::memory_order_acquire);
                if (inFlight) {
                    ImGui::BeginDisabled(true);
                }
                // Cheap rotating glyph spinner so the disabled-button state
                // doesn't look frozen during the multi-second local-model
                // transcribe pipeline. ImGui::GetTime() is in seconds; pick
                // one of four glyphs per ~120 ms.
                const char* spinnerGlyph = "|";
                if (inFlight) {
                    const int slot = static_cast<int>(::ImGui::GetTime() * 8.0) & 3;
                    spinnerGlyph = (slot == 0)   ? "|"
                                   : (slot == 1) ? "/"
                                   : (slot == 2) ? "-"
                                                 : "\\";
                }
                char e2eLabel[128];
                std::snprintf(e2eLabel, sizeof(e2eLabel),
                              inFlight ? "%s  %s"
                                       : "%s",
                              SmatchetLocalization::T("whisper.preferences.testE2E.button",
                                                      "Test end-to-end (capture 4 s + transcribe)"),
                              spinnerGlyph);
                if (ImGui::Button(e2eLabel)) {
                    s_e2eInFlight.store(true, std::memory_order_release);
                    s_e2eResult = "Recording 4 s — speak now...";
                    s_e2eResultType = 0;
                    MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;
                    TrackerConfig cfgSnap = d.cfg;
                    std::thread([cfgSnap, &dispatcher]() {
                        // --- Resolve route BEFORE spending 4 s capturing,
                        // so cloud-only with no key / local-only with no
                        // model fail fast.
                        const std::string requestedMode =
                            cfgSnap.WhisperMode.empty() ? std::string("auto") : cfgSnap.WhisperMode;
                        const std::string providerStr =
                            (cfgSnap.AiProviderKind == 0) ? std::string("openai")
                                                          : std::string("anthropic");
                        const std::string resolvedKey =
                            smatchet::whisper::pure::ResolveWhisperApiKey(
                                cfgSnap.WhisperApiKey, providerStr, cfgSnap.AiApiKey);
                        // Mirror ResolveWhisperModelDir from WhisperPlugin.cpp
                        // (anon namespace; inline here so Preferences stays
                        // self-contained).
                        std::string modelDir =
                            ConfigManager::GetPlatformSharedUserDataDirectory();
                        if (!modelDir.empty() && modelDir.back() != '/' &&
                            modelDir.back() != '\\') {
                            modelDir.push_back('/');
                        }
                        modelDir += "whisper";
                        const bool localPresent =
                            !cfgSnap.WhisperModel.empty() &&
                            smatchet::whisper::catalog::IsModelPresent(cfgSnap.WhisperModel,
                                                                        modelDir);

                        // Pick effective route per the user's spec:
                        //   - cloud: require key
                        //   - local: require model file present
                        //   - auto:  prefer cloud when key present, fall back to local
                        std::string effectiveMode;
                        std::string fastFail;
                        if (requestedMode == "cloud") {
                            if (resolvedKey.empty()) {
                                fastFail =
                                    "Cloud mode requires an API key. Set Whisper or AI API key "
                                    "(provider=openai) and retry.";
                            } else {
                                effectiveMode = "cloud";
                            }
                        } else if (requestedMode == "local") {
                            if (!localPresent) {
                                fastFail =
                                    "Local mode requires the selected model on disk. Open the "
                                    "model picker above and click Download first.";
                            } else {
                                effectiveMode = "local";
                            }
                        } else { // auto
                            if (!resolvedKey.empty()) {
                                effectiveMode = "cloud";
                            } else if (localPresent) {
                                effectiveMode = "local";
                            } else {
                                fastFail =
                                    "Auto mode needs either an API key (for cloud) or a "
                                    "downloaded local model. Neither was found.";
                            }
                        }
                        if (!fastFail.empty()) {
                            dispatcher.PostToMainThread([fastFail]() {
                                s_e2eInFlight.store(false, std::memory_order_release);
                                s_e2eResult = std::string("Failed: ") + fastFail;
                                s_e2eResultType = 2;
                            });
                            return;
                        }

                        // --- Capture once, route by effectiveMode below.
                        // Per-phase status updates keep the user informed —
                        // local-model transcription on medium.en can take
                        // 5+ seconds, and the "button disabled, nothing
                        // visible" silence was confusing.
                        const std::string modeForUi = effectiveMode;
                        dispatcher.PostToMainThread([modeForUi]() {
                            s_e2eResult = std::string("[1/3] Capturing 4 s (route=") +
                                          modeForUi + ") — speak now...";
                            s_e2eResultType = 0;
                        });
                        smatchet::whisper::WindowsAudioCapture cap;
                        std::string err;
                        if (!cap.Start(err)) {
                            const std::string e =
                                err.empty() ? std::string("capture start failed") : err;
                            dispatcher.PostToMainThread([e]() {
                                s_e2eInFlight.store(false, std::memory_order_release);
                                s_e2eResult = std::string("Failed (capture): ") + e;
                                s_e2eResultType = 2;
                            });
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(4));
                        cap.Stop();
                        std::vector<std::int16_t> pcm;
                        cap.DrainCapturedPcm(pcm);
                        dispatcher.PostToMainThread([modeForUi]() {
                            s_e2eResult = std::string("[2/3] Captured; transcribing via ") +
                                          modeForUi + "...";
                            s_e2eResultType = 0;
                        });
                        if (pcm.empty()) {
                            dispatcher.PostToMainThread([]() {
                                s_e2eInFlight.store(false, std::memory_order_release);
                                s_e2eResult =
                                    "Failed: 0 PCM samples (mic / consent / format issue — "
                                    "click Test microphone for narrower diagnosis)";
                                s_e2eResultType = 2;
                            });
                            return;
                        }

                        const std::string lang =
                            cfgSnap.WhisperLanguage.empty() ? std::string("en")
                                                            : cfgSnap.WhisperLanguage;
                        const std::size_t samples = pcm.size();
                        std::string text;
                        std::string txErr;
                        bool ok = false;

                        if (effectiveMode == "cloud") {
                            // Encode + POST.
                            const std::vector<std::uint8_t> wav =
                                smatchet::whisper::pure::EncodeWav(
                                    pcm,
                                    smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate,
                                    1);
                            smatchet::whisper::WhisperApiClient client;
                            ok = client.Transcribe(wav, resolvedKey, lang, text, txErr);
                        } else {
                            // Local — WhisperLocal::LoadModel + Transcribe on the
                            // int16 overload (matches the worker pipeline path).
                            // When SMATCHET_WHISPER_LOCAL_BACKEND=OFF the helper
                            // returns "local backend not built", which we surface
                            // verbatim so the user knows to flip the sub-option.
                            const std::string modelPath =
                                modelDir + "/" + cfgSnap.WhisperModel + ".bin";
                            smatchet::whisper::WhisperLocal local;
                            std::string loadErr;
                            if (!local.LoadModel(modelPath, loadErr)) {
                                txErr = std::string("LoadModel: ") + loadErr;
                                ok = false;
                            } else {
                                ok = local.Transcribe(pcm, lang, text, txErr);
                            }
                        }

                        const std::string mode = effectiveMode;
                        dispatcher.PostToMainThread([ok, txErr, text, samples, mode]() {
                            s_e2eInFlight.store(false, std::memory_order_release);
                            char buf[1024];
                            if (!ok) {
                                std::snprintf(buf, sizeof(buf),
                                              "[3/3] Failed (%s, transcribe): captured %zu "
                                              "samples; %s",
                                              mode.c_str(), samples, txErr.c_str());
                                s_e2eResultType = 2;
                            } else if (text.empty()) {
                                std::snprintf(buf, sizeof(buf),
                                              "[3/3] Warn (%s): captured %zu samples + "
                                              "transcribe ok, but empty result (silence at "
                                              "recogniser)",
                                              mode.c_str(), samples);
                                s_e2eResultType = 2;
                            } else {
                                std::snprintf(buf, sizeof(buf),
                                              "[3/3] OK (%s, %zu samples): \"%s\"",
                                              mode.c_str(), samples, text.c_str());
                                s_e2eResultType = 1;
                            }
                            s_e2eResult = buf;
                        });
                    }).detach();
                }
                if (inFlight) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(records 4 s + uploads to whisper for end-to-end check)");
                if (!s_e2eResult.empty()) {
                    ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
                    if (s_e2eResultType == 1) col = ImVec4(0.35f, 0.90f, 0.45f, 1.0f);
                    if (s_e2eResultType == 2) col = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
                    ImGui::TextColored(col, "%s", s_e2eResult.c_str());
                }
            }

            // --- Phase F language / trim / max-clip / auto-send rows. ---
            ImGui::Separator();
            ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.language.label", "Language:"));
            ImGui::SameLine();
            {
                // Static set of common ISO codes the bundled English models +
                // multilingual cloud both understand. "auto" asks the backend
                // to detect. Users with a multilingual local model can add
                // more codes via direct config edit; the dropdown captures
                // the common 80 % rather than enumerating every supported tag.
                static const char* kLanguageCodes[] = {
                    "en", "auto", "fr", "de", "es", "it", "pt", "nl", "ja", "zh", "ko", "ru", "pl", "ar", "hi", "tr",
                };
                int langIdx = 0;
                for (int i = 0; i < static_cast<int>(sizeof(kLanguageCodes) / sizeof(kLanguageCodes[0])); ++i) {
                    if (d.cfg.WhisperLanguage == kLanguageCodes[i]) {
                        langIdx = i;
                        break;
                    }
                }
                if (ImGui::Combo("##WhisperLanguage", &langIdx, kLanguageCodes,
                                 static_cast<int>(sizeof(kLanguageCodes) / sizeof(kLanguageCodes[0])))) {
                    d.cfg.WhisperLanguage = kLanguageCodes[langIdx];
                    MarkPrefsDirty(d);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "%s", SmatchetLocalization::T("whisper.preferences.language.autoHint", "(or \"auto\" for autodetect)"));

            if (ImGui::Checkbox(
                    SmatchetLocalization::T("whisper.preferences.trim.label", "Trim leading/trailing silence"),
                    &d.cfg.WhisperTrim)) {
                MarkPrefsDirty(d);
            }

            // Max clip length: int input with explicit clamp on edit. 0 = unlimited.
            {
                int maxClip = d.cfg.WhisperMaxClipSec;
                ImGui::TextUnformatted(
                    SmatchetLocalization::T("whisper.preferences.maxClipSec.label", "Max clip length:"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::InputInt("##WhisperMaxClipSec", &maxClip, 0, 0)) {
                    if (maxClip < 0)
                        maxClip = 0;
                    if (maxClip > 600)
                        maxClip = 600;
                    d.cfg.WhisperMaxClipSec = maxClip;
                    MarkPrefsDirty(d);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", SmatchetLocalization::T("whisper.preferences.maxClipSec.unit", "seconds"));
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "%s", SmatchetLocalization::T("whisper.preferences.maxClipSec.hint", "(0 = unlimited; max 600)"));
            }

            if (ImGui::Checkbox(SmatchetLocalization::T("whisper.preferences.autoSend.label",
                                                        "Auto-send AI chat on punctuation (\".\", \"!\", \"?\")"),
                                &d.cfg.WhisperAutoSendOnPunctuation)) {
                MarkPrefsDirty(d);
            }

            // Privacy disclosure — three-bullet list.
            ImGui::Separator();
            ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.privacyHeading", "Privacy disclosure"));
            ImGui::BulletText("%s", SmatchetLocalization::T("whisper.preferences.privacyLocal",
                                                            "Local mode: audio stays on your machine; "
                                                            "no network call is made."));
            ImGui::BulletText("%s", SmatchetLocalization::T("whisper.preferences.privacyCloud",
                                                            "Cloud mode: audio is uploaded to OpenAI "
                                                            "for transcription."));
            ImGui::BulletText("%s", SmatchetLocalization::T("whisper.preferences.privacyDisabled",
                                                            "Disabled: no microphone access, no "
                                                            "network call, no model download."));

            // Phase F — "Re-run setup banner" debug helper. Flips
            // WhisperSetupCompleted back to false so the first-run banner
            // appears on next launch. Useful for QA / repro flows; not for
            // routine users (no harm if pressed — the banner just re-asks
            // the consent question).
            ImGui::Separator();
            if (ImGui::Button(
                    SmatchetLocalization::T("whisper.preferences.rerunSetup.button", "Re-run setup banner"))) {
                d.cfg.WhisperSetupCompleted = false;
                d.cfg.WhisperSetupChoice.clear();
                MarkPrefsDirty(d);
                LOG_INFO("Whisper preferences: Re-run setup banner pressed — "
                         "WhisperSetupCompleted reset; banner returns next launch");
            }
            ImGui::SetItemTooltip("%s", SmatchetLocalization::T("whisper.preferences.rerunSetup.tooltip",
                                                                "Forces WhisperSetupCompleted=false; "
                                                                "banner appears next launch"));

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
