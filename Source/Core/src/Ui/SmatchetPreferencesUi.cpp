// Define NOMINMAX / WIN32_LEAN_AND_MEAN before any include that transitively
// pulls in <windows.h> (SQLiteCpp via AppController.h chain) — otherwise the
// preprocessor defines `min` / `max` as macros and `std::min(...)` later in
// this TU fails to parse. Mirrors the same dance in AppController.cpp.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "SmatchetUI.h"
#include "SmatchetPreferencesUi_detail.h"

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
#include "EmailMaskForLog.h"
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
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
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

// MaskEmailForLog (#820 PII redaction) now lives in the standalone pure header
// "EmailMaskForLog.h" (smatchet::logging::pure) so the doctest rig can cover it
// without this Ui TU's ImGui / cpr / AppController dependency chain. The call
// site below uses the namespaced name.

} // namespace

void SmatchetUI::resetPreferencesWindowState(UiDrawSession& d) {
    d.preferencesBuffersLoaded = false;
    d.mcpPrefsSavedHintUntil = {};
    preferencesState_.templateFlags = SmatchetPreferencesUiTemplateFlags{};
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
}

bool SmatchetUI::beginPreferencesWindow(UiDrawSession& d) {
    const bool wantFocus = d.requestPreferencesFocus;
    // wantFocus OR layoutForceDefaultsFrames forces SetNextWindowFocus before Begin — the only
    // path that activates a docked tab. Post-Begin SetWindowFocus is belt-and-braces for floating.
    prepareTopLevelWindow(d, "preferences", 560.0f, 480.0f, wantFocus || d.layoutForceDefaultsFrames > 0);
    if (!ImGui::Begin("Preferences", &d.showPreferences)) {
        if (wantFocus) {
            d.requestPreferencesFocus = false;
        }
        ImGui::End();
        return false;
    }
    repairTopLevelWindow(d, "preferences", 420.0f, 360.0f);
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestPreferencesFocus = false;
        LOG_DEBUG("Preferences window: focused via menu request");
    }
    return true;
}

void SmatchetUI::loadPreferencesBuffers(UiDrawSession& d) {
    if (d.preferencesBuffersLoaded) {
        return;
    }
    CopyStringToBuffer(d.domainBuf, d.cfg.Domain);
    CopyStringToBuffer(d.emailBuf, d.cfg.Email);
    CopyStringToBuffer(d.tokenBuf, d.cfg.ApiToken);
    // PR 6: projectKeyBuf / planeProjectBuf removed — see SmatchetUiSession.h.
    CopyStringToBuffer(d.trackerTypeBuf, d.cfg.TrackerType);
    CopyStringToBuffer(d.planeUrlBuf, d.cfg.PlaneUrl);
    CopyStringToBuffer(d.planeWorkspaceBuf, d.cfg.PlaneWorkspaceSlug);
    CopyStringToBuffer(d.planeApiKeyBuf, d.cfg.PlaneApiKey);
    CopyStringToBuffer(d.githubBaseUrlBuf,
                       d.cfg.GitHubBaseUrl.empty() ? std::string("https://api.github.com") : d.cfg.GitHubBaseUrl);
    CopyStringToBuffer(d.githubPatBuf, d.cfg.GitHubPat);
    CopyStringToBuffer(d.githubOwnerBuf, d.cfg.GitHubOwner);
    CopyStringToBuffer(d.githubRepoBuf, d.cfg.GitHubRepo);
    CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
    CopyStringToBuffer(d.newIssueInheritFieldsPlaneBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsPlane));
    CopyStringToBuffer(d.newIssueInheritFieldsGitHubBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsGitHub));
#if defined(SMATCHET_WITH_MCP)
    d.mcpEnabled = d.cfg.McpEnabled;
    d.mcpPort = d.cfg.McpPort;
    d.mcpAllowRemote = d.cfg.McpAllowRemote;
    d.mcpAllowLuaExecution = d.cfg.McpAllowLuaExecution;
    CopyStringToBuffer(d.mcpAuthTokenBuf, d.cfg.McpAuthToken);
#endif
    d.preferencesBuffersLoaded = true;
}

namespace {

// Backend-selection section of the Tracker tab: read-only toggle + the Jira/Plane/GitHub combo.
// Returns the selected backend index (0=Jira, 1=Plane, 2=GitHub). Extracted from
// drawPreferencesTrackerTab during the over-100-line decomposition; behaviour-identical.
int DrawTrackerBackendSelection(UiDrawSession& d) {
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

    const char* items[] = {"Jira", "Plane", "GitHub"};
    int currentItem = 0;
    {
        // Defensive case-insensitive match against persisted config —
        // smatchet_config.json could be hand-edited with lowercase
        // "plane"/"github" values; the combo writer below always
        // emits canonical PascalCase, but the load path doesn't
        // canonicalize. CR finding on PR #386/#387.
        const std::string trackerTypeStr(d.trackerTypeBuf);
        if (trackerTypeStr == "Plane" || trackerTypeStr == "plane") {
            currentItem = 1;
        } else if (trackerTypeStr == "GitHub" || trackerTypeStr == "github") {
            currentItem = 2;
        }
    }
    if (ImGui::Combo("Tracker Backend", &currentItem, items, IM_ARRAYSIZE(items))) {
        CopyStringToBuffer(d.trackerTypeBuf, items[currentItem]);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    return currentItem;
}

// Per-backend credential/config inputs of the Tracker tab (Jira / Plane / GitHub). Extracted from
// drawPreferencesTrackerTab during the over-100-line decomposition; behaviour-identical.
void DrawTrackerBackendConfig(UiDrawSession& d, int currentItem) {
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
        ImGui::SetItemTooltip("Comma-separated Jira field ids copied from the last grid row when you click + New issue "
                              "(e.g. description, priority, assignee, labels, components).");
    } else if (currentItem == 1) {
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
    } else {
        // GitHub-as-tracker — PR3 of docs/plans/shipped/github-tracker-backend.md.
        ImGui::TextUnformatted("GitHub Configuration (github.com or Enterprise)");
        ImGui::InputText("Base URL", d.githubBaseUrlBuf, sizeof(d.githubBaseUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. https://api.github.com or https://github.your-corp.com/api/v3");
        ImGui::InputText("Personal Access Token", d.githubPatBuf, sizeof(d.githubPatBuf), ImGuiInputTextFlags_Password);
        ImGui::SetItemTooltip("Fine-grained PAT with repo + issues + projects (read/write) scope.");
        ImGui::InputText("Owner", d.githubOwnerBuf, sizeof(d.githubOwnerBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("GitHub user or organization, e.g. \"alexandrosk0\".");
        ImGui::InputText("Repo", d.githubRepoBuf, sizeof(d.githubRepoBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Repository name, e.g. \"Smatchet\". Combined with Owner: fetches issues from "
                              "github.com/<owner>/<repo>. Leave both empty for cross-repo /search/issues (PR4).");
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (GitHub)", d.newIssueInheritFieldsGitHubBuf,
                         sizeof(d.newIssueInheritFieldsGitHubBuf));
        ImGui::SetItemTooltip(
            "Comma-separated GitHub field ids copied from the last grid row when you click + New issue "
            "(e.g. body, labels, assignees, milestone).");
    }
    ImGui::Spacing();
}

// "Recently used projects" section of the Tracker tab: cached-project list filtered to the current
// backend + endpoint, each with a Forget button. Extracted from drawPreferencesTrackerTab during the
// over-100-line decomposition; behaviour-identical.
void DrawTrackerRecentProjects(UiDrawSession& d, int currentItem) {
    // PR 6: "Recently used projects" — read-only listbox sourced from FieldCatalogCache,
    // filtered to the current backend + endpoint. Replaces the deleted "Project Key" /
    // "Project ID (UUID)" preference rows. Each row has a Forget button.
    ImGui::Separator();
    ImGui::TextUnformatted(SmatchetLocalization::T("prefs.recentProjects", "Recently used projects"));
    std::string backendKind;
    std::string endpoint;
    if (currentItem == 1) {
        backendKind = "Plane";
        endpoint = std::string(d.planeUrlBuf) + std::string("|") + std::string(d.planeWorkspaceBuf);
    } else if (currentItem == 2) {
        backendKind = "GitHub";
        endpoint = std::string(d.githubBaseUrlBuf) + std::string("|") + std::string(d.githubOwnerBuf) +
                   std::string("/") + std::string(d.githubRepoBuf);
    } else {
        backendKind = "Jira";
        endpoint = std::string(d.domainBuf);
    }
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
    ImGui::Spacing();
}

} // namespace

void SmatchetUI::drawPreferencesTrackerTab(UiDrawSession& d) {
    if (!ImGui::BeginTabItem("Tracker")) {
        return;
    }
    const int currentItem = DrawTrackerBackendSelection(d);
    DrawTrackerBackendConfig(d, currentItem);
    DrawTrackerRecentProjects(d, currentItem);
    ImGui::TextWrapped("Query/JQL and column fields are configured in the Views dashboard.");
    if (ImGui::Button("Open Views Dashboard")) {
        d.showViewsDashboard = true;
        d.requestViewsDashboardFocus = true;
    }
    ImGui::EndTabItem();
}

#if defined(SMATCHET_WITH_MCP)
void SmatchetUI::drawPreferencesIntegrationsTab(AppController& app, UiDrawSession& d) {
    if (!ImGui::BeginTabItem("Integrations")) {
        return;
    }
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
    ImGui::TextDisabled(
        "Runtime status, endpoints, and action log: Automation -> Agent Bridge (MCP)... (separate window).");
    ImGui::EndTabItem();
}
#endif

namespace {

// Parse a comma-separated "inherit fields from last row" buffer into `out` (dropping empties and
// the implicit "summary"), fall back to the default set when empty, and canonicalize the buffer
// back to the joined form. Collapses the three identical Jira/Plane/GitHub blocks that previously
// inlined this in onPreferencesSaveAndSync (over-100-line decomposition); behaviour-identical.
template <std::size_t N> void ApplyInheritFieldsBuf(char (&buf)[N], std::vector<std::string>& out) {
    std::vector<std::string> parsedInherit = ParseCsv(std::string(buf));
    out.clear();
    for (const auto& s : parsedInherit) {
        if (!s.empty() && s != "summary") {
            out.push_back(s);
        }
    }
    if (out.empty()) {
        out = IssueDraftHelpers::DefaultNewIssueInheritFieldIds();
    }
    CopyStringToBuffer(buf, JoinCsv(out));
}

} // namespace

void SmatchetUI::onPreferencesSaveAndSync(AppController& app, UiDrawSession& d) {
    d.cfg.Domain = d.domainBuf;
    d.cfg.Email = d.emailBuf;
    d.cfg.ApiToken = d.tokenBuf;
    // PR 6: ProjectKey / PlaneProjectId writebacks removed — project is per-operation.
    // Canonicalize so a hand-edited lowercase "plane"/"github" buffer value
    // persists as the canonical PascalCase form the rest of the code (and the
    // exact-match TrackerType == "Plane"/"GitHub" branches below) expect. The
    // same normalizer is applied to TrackerType a few lines down. Issue #820.
    d.cfg.TrackerType = ConfigManager::NormalizeViewsBackendKey(std::string(d.trackerTypeBuf));
    d.cfg.PlaneUrl = d.planeUrlBuf;
    d.cfg.PlaneWorkspaceSlug = d.planeWorkspaceBuf;
    d.cfg.PlaneApiKey = d.planeApiKeyBuf;
    d.cfg.GitHubBaseUrl = d.githubBaseUrlBuf;
    d.cfg.GitHubPat = d.githubPatBuf;
    d.cfg.GitHubOwner = d.githubOwnerBuf;
    d.cfg.GitHubRepo = d.githubRepoBuf;
    // Issue #979 — trim leading/trailing whitespace on every credential/identity field
    // BEFORE the base-URL empty-default below, so a whitespace-only buffer still gets the
    // default. A trailing space in the Jira email made Atlassian 401 every request.
    SmatchetPreferencesUiDetail::TrimTrackerCredentialFields(d.cfg);
    if (d.cfg.GitHubBaseUrl.empty()) {
        d.cfg.GitHubBaseUrl = "https://api.github.com";
    }
    ApplyInheritFieldsBuf(d.newIssueInheritFieldsBuf, d.cfg.NewIssueInheritFieldIds);
    ApplyInheritFieldsBuf(d.newIssueInheritFieldsPlaneBuf, d.cfg.NewIssueInheritFieldIdsPlane);
    ApplyInheritFieldsBuf(d.newIssueInheritFieldsGitHubBuf, d.cfg.NewIssueInheritFieldIdsGitHub);
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
    } else if (d.cfg.TrackerType == "GitHub") {
        LOG_INFO("Updated tracker config (GitHub). BaseUrl='%s' Owner='%s' Repo='%s' (PAT length=%zu)",
                 d.cfg.GitHubBaseUrl.c_str(), d.cfg.GitHubOwner.c_str(), d.cfg.GitHubRepo.c_str(),
                 d.cfg.GitHubPat.size());
    } else {
        LOG_INFO("Updated tracker config (Jira). Domain='%s', Email='%s'", d.cfg.Domain.c_str(),
                 smatchet::logging::pure::MaskEmailForLog(d.cfg.Email).c_str());
    }
    d.triggerCatalogRefetch = true;
    const std::string oldBackend = d.lastViewsBackendKey; // session-level (review HIGH-4)
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

void SmatchetUI::drawPreferencesWindow(AppController& app, UiDrawSession& d) {
    if (!d.showPreferences) {
        resetPreferencesWindowState(d);
        return;
    }

    if (!beginPreferencesWindow(d)) {
        return;
    }

    loadPreferencesBuffers(d);

    if (ImGui::BeginTabBar("PreferencesTabs")) {
        drawPreferencesTrackerTab(d);
#if defined(SMATCHET_WITH_MCP)
        drawPreferencesIntegrationsTab(app, d);
#endif
#if defined(SMATCHET_WITH_AI)
        DrawAssistantPreferencesTab(app, d);
#endif
#if defined(SMATCHET_WITH_WHISPER)
        DrawWhisperPreferencesTab(app, d);
#endif
        DrawLocalAndAppearancePreferencesTabs(*this, app, d);
        DrawTemplatePreferencesTabs(*this, app, d, preferencesState_.templateFlags);
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "Save & Sync writes the Tracker tab (and optional Integrations tab when enabled in this build) to "
        "disk and refreshes the tracker connection. MCP runtime status: Automation -> Agent Bridge (MCP).... "
        "Appearance options save immediately when changed. Log level and verbose logging: Inspect -> Runtime Log. The "
        "Annotate "
        "Analysis tab has its own Save "
        "settings and Reload settings buttons.");
    ImGui::Spacing();
    if (ImGui::Button("Save & Sync", ImVec2(140.0f, 0.0f))) {
        onPreferencesSaveAndSync(app, d);
    }

    ImGui::End();
}
