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
#include "SmatchetHelpMarker.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "Ui/SmatchetIconButtons.h"

#include "IconsFontAwesome6.h"
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
    // Pillar 2 (#892): drop the Tracker-tab open-edge latch so reopening the window re-snapshots
    // the cached-project list once (instead of re-reading disk every frame the tab is visible).
    d.prefsTrackerTabWasOpen = false;
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
    // Reopening the window is a Discard-on-close: revert BOTH the working copy
    // (workingSeeded=false re-seeds it from cfg) AND the InputText buffers
    // (forceReseed=true — #1706: clearing only workingSeeded left the
    // function-static s_bufs buffers seeded, so the fields kept displaying the
    // discarded edit text). See ResetAssistantPrefsSeedLatchesOnClose (tested in
    // tests/Core/PreferencesAssistantWorkingCopy.test.cpp).
    SmatchetPreferencesUiDetail::ResetAssistantPrefsSeedLatchesOnClose(d.assistantPrefsWorkingSeeded,
                                                                       d.assistantPrefsForceBufferReseed);
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
    // No projectKeyBuf / planeProjectBuf — see SmatchetUiSession.h.
    CopyStringToBuffer(d.trackerTypeBuf, d.cfg.TrackerType);
    CopyStringToBuffer(d.planeUrlBuf, d.cfg.PlaneUrl);
    CopyStringToBuffer(d.planeWorkspaceBuf, d.cfg.PlaneWorkspaceSlug);
    CopyStringToBuffer(d.planeApiKeyBuf, d.cfg.PlaneApiKey);
    CopyStringToBuffer(d.githubBaseUrlBuf,
                       d.cfg.GitHubBaseUrl.empty() ? std::string("https://api.github.com") : d.cfg.GitHubBaseUrl);
    CopyStringToBuffer(d.githubPatBuf, d.cfg.GitHubPat);
    CopyStringToBuffer(d.githubOwnerBuf, d.cfg.GitHubOwner);
    CopyStringToBuffer(d.githubRepoBuf, d.cfg.GitHubRepo);
    CopyStringToBuffer(d.linearApiKeyBuf, d.cfg.LinearApiKey);
    CopyStringToBuffer(d.linearBaseUrlBuf, d.cfg.LinearBaseUrl.empty() ? std::string("https://api.linear.app/graphql")
                                                                       : d.cfg.LinearBaseUrl);
    CopyStringToBuffer(d.linearTeamKeyBuf, d.cfg.LinearTeamKey);
    CopyStringToBuffer(d.linearTeamIdBuf, d.cfg.LinearTeamId);
    CopyStringToBuffer(d.linearWorkspaceUrlBuf, d.cfg.LinearWorkspaceUrl);
    CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
    CopyStringToBuffer(d.newIssueInheritFieldsPlaneBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsPlane));
    CopyStringToBuffer(d.newIssueInheritFieldsGitHubBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsGitHub));
    CopyStringToBuffer(d.newIssueInheritFieldsLinearBuf, JoinCsv(d.cfg.NewIssueInheritFieldIdsLinear));
    CopyStringToBuffer(d.gitCommitReposBuf, d.cfg.GitCommitRepos);
    CopyStringToBuffer(d.productionGroupKeywordBuf, d.cfg.ProductionGroupKeyword);
    d.userActivityDayWindow = d.cfg.UserActivityDayWindow;
    d.maxUserChanges = d.cfg.MaxUserChanges;
    d.vcsFeedLayoutIndex = (d.cfg.VcsFeedLayout == "separate") ? 1 : 0;
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

// Backend-selection section of the Tracker tab: read-only toggle + the Jira/Plane/GitHub/Linear combo.
// Returns the selected backend index (0=Jira, 1=Plane, 2=GitHub, 3=Linear). Extracted from
// drawPreferencesTrackerTab during the over-100-line decomposition; behaviour-identical.
int DrawTrackerBackendSelection(UiDrawSession& d) {
    ImGui::TextUnformatted("Backend Selection");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Read-only mode", &d.cfg.ReadOnlyMode)) {
        MarkPrefsDirty(d);
    }
    ImGui::SetItemTooltip("Disables all tracker-changing actions.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.tracker.read_only.help",
                               "Disables tracker-changing actions such as field edits, issue creation, "
                               "comments, worklogs, and offline write replay. Enabled by default on first "
                               "launch before setup.");
    ImGui::Spacing();

    const char* items[] = {"Jira", "Plane", "GitHub", "Linear"};
    int currentItem = 0;
    {
        // Defensive case-insensitive match against persisted config —
        // smatchet_config.json could be hand-edited with lowercase
        // "plane"/"github"/"linear" values; the combo writer below always
        // emits canonical PascalCase, but the load path doesn't
        // canonicalize.
        const std::string trackerTypeStr(d.trackerTypeBuf);
        if (trackerTypeStr == "Plane" || trackerTypeStr == "plane") {
            currentItem = 1;
        } else if (trackerTypeStr == "GitHub" || trackerTypeStr == "github") {
            currentItem = 2;
        } else if (trackerTypeStr == "Linear" || trackerTypeStr == "linear") {
            currentItem = 3;
        }
    }
    if (d.effectiveUiMode == EffectiveUiMode::Mobile) {
        // P1.5 touch-first backend picker: when the mobile shell renders this Preferences page
        // (effectiveUiMode == Mobile — phone, or a narrow desktop window pinned/auto-resolved to
        // Mobile), replace the desktop combo's small popup hit-targets with full-width selectable
        // rows (the drawer page-list touch idiom). Writes the same d.trackerTypeBuf the combo does,
        // so DrawTrackerBackendConfig below and the multi-backend ITrackerBackend layer
        // (Jira/Plane/GitHub/Linear) are inherited unchanged — this is a widget swap, not new
        // backend code. The density-scaled mobile style already enlarges each Selectable to a
        // touch-comfortable target, so no explicit row height is needed.
        ImGui::TextUnformatted("Tracker Backend");
        for (int i = 0; i < IM_ARRAYSIZE(items); ++i) {
            const bool selected = (i == currentItem);
            if (ImGui::Selectable(items[i], selected) && i != currentItem) {
                currentItem = i;
                CopyStringToBuffer(d.trackerTypeBuf, items[i]);
            }
        }
    } else if (ImGui::Combo("Tracker Backend", &currentItem, items, IM_ARRAYSIZE(items))) {
        CopyStringToBuffer(d.trackerTypeBuf, items[currentItem]);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    return currentItem;
}

// Per-backend credential/config inputs of the Tracker tab (Jira / Plane / GitHub / Linear). Extracted
// from drawPreferencesTrackerTab during the over-100-line decomposition; behaviour-identical.
void DrawTrackerBackendConfig(UiDrawSession& d, int currentItem) {
#if !defined(_WIN32) && !defined(__ANDROID__)
    // ConfigManager's DPAPI secret protection is Win32-only (ConfigManager_PathUtils.cpp
    // ProtectSecretForConfig is a #else passthrough on every other platform), so on a desktop
    // build without a platform secret store (Linux/macOS) the tracker API token persists as
    // plaintext in the app's private config file. Warn explicitly there. Android is excluded:
    // Phase-1 P1.0 seals the token via the Keystore-backed SetSecretCryptoOverride seam.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.30f, 1.0f));
    ImGui::TextWrapped("Note: on this platform the API token is stored unencrypted in the app's private "
                       "storage. Use a scoped, revocable token.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
#endif
    if (currentItem == 0) {
        ImGui::TextUnformatted("Jira Configuration (Atlassian Cloud)");
        ImGui::InputText("Domain", d.domainBuf, sizeof(d.domainBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. companyname.atlassian.net");
        ImGui::InputText("Email", d.emailBuf, sizeof(d.emailBuf));
        ImGui::InputText("API Token", d.tokenBuf, sizeof(d.tokenBuf), ImGuiInputTextFlags_Password);
        // No "Project Key" preference row. Project is per-operation — picked
        // via the new-issue draft picker, derived from the active view's JQL, or supplied
        // on ticket.create. The "Recently used projects" section below surfaces cached
        // projects for visibility / Forget.
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (Jira)", d.newIssueInheritFieldsBuf,
                         sizeof(d.newIssueInheritFieldsBuf));
        ImGui::SetItemTooltip("Comma-separated Jira field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_jira.help",
                                   "Comma-separated Jira field ids copied from the last grid row when you "
                                   "click + New issue (e.g. description, priority, assignee, labels, "
                                   "components).");
    } else if (currentItem == 1) {
        ImGui::TextUnformatted("Plane Configuration (plane.so)");
        ImGui::InputText("URL", d.planeUrlBuf, sizeof(d.planeUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. https://api.plane.so");
        ImGui::InputText("Workspace Slug", d.planeWorkspaceBuf, sizeof(d.planeWorkspaceBuf),
                         ImGuiInputTextFlags_CharsNoBlank);
        // No "Project ID (UUID)" preference row. See Jira note above.
        ImGui::InputText("API Key", d.planeApiKeyBuf, sizeof(d.planeApiKeyBuf), ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (Plane)", d.newIssueInheritFieldsPlaneBuf,
                         sizeof(d.newIssueInheritFieldsPlaneBuf));
        ImGui::SetItemTooltip("Comma-separated Plane field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_plane.help",
                                   "Comma-separated Plane field ids copied from the last grid row when you "
                                   "click + New issue (e.g. description, priority, assignee, labels).");
    } else if (currentItem == 2) {
        // GitHub-as-tracker — docs/plans/shipped/github-tracker-backend.md.
        ImGui::TextUnformatted("GitHub Configuration (github.com or Enterprise)");
        ImGui::InputText("Base URL", d.githubBaseUrlBuf, sizeof(d.githubBaseUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. https://api.github.com or https://github.your-corp.com/api/v3");
        ImGui::InputText("Personal Access Token", d.githubPatBuf, sizeof(d.githubPatBuf), ImGuiInputTextFlags_Password);
        ImGui::SetItemTooltip("Fine-grained PAT with repo + issues + projects (read/write) scope.");
        ImGui::InputText("Owner", d.githubOwnerBuf, sizeof(d.githubOwnerBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("GitHub user or organization, e.g. \"alexandrosk0\".");
        ImGui::InputText("Repo", d.githubRepoBuf, sizeof(d.githubRepoBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Repository name, e.g. \"Smatchet\".");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.github_repo.help",
                                   "Repository name, e.g. \"Smatchet\". Combined with Owner: fetches issues "
                                   "from github.com/<owner>/<repo>. Leave both empty for cross-repo "
                                   "/search/issues.");
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (GitHub)", d.newIssueInheritFieldsGitHubBuf,
                         sizeof(d.newIssueInheritFieldsGitHubBuf));
        ImGui::SetItemTooltip("Comma-separated GitHub field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_github.help",
                                   "Comma-separated GitHub field ids copied from the last grid row when you "
                                   "click + New issue (e.g. body, labels, assignees, milestone).");
    } else {
        // Linear-as-tracker — Slice 1 of docs/plans/linear-tracker-backend.md. Draft scope is
        // the Team, so identity is the Team Key / Team Id pair (mirrors GitHub's Owner/Repo shape).
        ImGui::TextUnformatted("Linear Configuration (linear.app)");
        ImGui::InputText("API Key", d.linearApiKeyBuf, sizeof(d.linearApiKeyBuf), ImGuiInputTextFlags_Password);
        ImGui::SetItemTooltip("Personal API Key from Linear Settings -> API.");
        ImGui::InputText("Team Key", d.linearTeamKeyBuf, sizeof(d.linearTeamKeyBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Team key, e.g. \"ENG\" (the TEAM-123 issue prefix).");
        ImGui::InputText("Team", d.linearTeamIdBuf, sizeof(d.linearTeamIdBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Linear team UUID. Optional — resolved from Team Key when empty.");
        ImGui::InputText("Base URL", d.linearBaseUrlBuf, sizeof(d.linearBaseUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. https://api.linear.app/graphql");
        ImGui::InputText("Workspace URL", d.linearWorkspaceUrlBuf, sizeof(d.linearWorkspaceUrlBuf),
                         ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Optional workspace URL / display hint.");
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (Linear)", d.newIssueInheritFieldsLinearBuf,
                         sizeof(d.newIssueInheritFieldsLinearBuf));
        ImGui::SetItemTooltip("Comma-separated Linear field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_linear.help",
                                   "Comma-separated Linear field ids copied from the last grid row when you "
                                   "click + New issue (e.g. description, priority, assignee, labels).");
    }
    ImGui::Spacing();
}

// "Recently used projects" section of the Tracker tab: cached-project list filtered to the current
// backend + endpoint, each with a Forget button. Extracted from drawPreferencesTrackerTab during the
// over-100-line decomposition; behaviour-identical.
void DrawTrackerRecentProjects(UiDrawSession& d, int currentItem) {
    // "Recently used projects" — read-only listbox sourced from FieldCatalogCache,
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
    } else if (currentItem == 3) {
        backendKind = "Linear";
        endpoint = std::string(d.linearBaseUrlBuf) + std::string("|") + std::string(d.linearTeamIdBuf);
    } else {
        backendKind = "Jira";
        endpoint = std::string(d.domainBuf);
    }
    // Pillar 2 (#892): read the open-edge snapshot (refreshed in drawPreferencesTrackerTab)
    // instead of re-reading disk every frame. Copy so the per-backend filter below is local.
    std::vector<FieldCatalogCache::CachedProjectEntry> cached = d.cachedProjectsSnapshot;
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
            // field today; a future change may extend the schema with one — for now the key alone
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

// User Info window / VCS commit feed settings (docs/plans/user-info-window.md). Immediate-dirty
// (MCP-style): clamp + compare-against-cfg, write all fields and mark prefs dirty when any changed —
// no Save button. These keys were config-only (not on the config.set allowlist, hand-edited JSON)
// until exposed here.
void DrawUserInfoFeedSettings(UiDrawSession& d) {
    ImGui::TextUnformatted("User Info & commit feed");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::InputText("Git commit repos", d.gitCommitReposBuf, sizeof(d.gitCommitReposBuf));
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.userinfo.git_commit_repos.help",
                               "Comma-separated owner/repo list the GitHub commit feed queries in the User "
                               "Info window (e.g. \"alexandrosk0/Smatchet\"). Leave empty to reuse the "
                               "tracker's own Owner/Repo. Auth reuses the GitHub PAT.");
    ImGui::InputText("Production group keyword", d.productionGroupKeywordBuf, sizeof(d.productionGroupKeywordBuf));
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.userinfo.production_group.help",
                               "Case-insensitive substring marking a tracker user-group as \"production\" "
                               "(e.g. \"prod\"). Empty disables the production-group highlight.");
    ImGui::InputInt("Activity day window", &d.userActivityDayWindow);
    if (d.userActivityDayWindow < 1) {
        d.userActivityDayWindow = 1;
    }
    ImGui::SetItemTooltip("How many days back the activity feed reaches (>= 1).");
    ImGui::InputInt("Max changes per source", &d.maxUserChanges);
    if (d.maxUserChanges < 1) {
        d.maxUserChanges = 1;
    }
    ImGui::SetItemTooltip("Max submitted changes fetched per VCS source (>= 1).");
    const char* kLayouts[] = {"unified", "separate"};
    ImGui::Combo("VCS feed layout", &d.vcsFeedLayoutIndex, kLayouts, IM_ARRAYSIZE(kLayouts));
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.userinfo.vcs_layout.help",
                               "unified: commits from all sources merged newest-first. separate: one "
                               "section per VCS source.");
    const std::string reposBuf(d.gitCommitReposBuf);
    const std::string prodBuf(d.productionGroupKeywordBuf);
    const std::string layout = (d.vcsFeedLayoutIndex == 1) ? "separate" : "unified";
    const bool dirty = reposBuf != d.cfg.GitCommitRepos || prodBuf != d.cfg.ProductionGroupKeyword ||
                       d.userActivityDayWindow != d.cfg.UserActivityDayWindow ||
                       d.maxUserChanges != d.cfg.MaxUserChanges || layout != d.cfg.VcsFeedLayout;
    if (dirty) {
        d.cfg.GitCommitRepos = reposBuf;
        d.cfg.ProductionGroupKeyword = prodBuf;
        d.cfg.UserActivityDayWindow = d.userActivityDayWindow;
        d.cfg.MaxUserChanges = d.maxUserChanges;
        d.cfg.VcsFeedLayout = layout;
        MarkPrefsDirty(d);
    }
    ImGui::Spacing();
}

} // namespace

void SmatchetUI::drawPreferencesTrackerTab(UiDrawSession& d) {
    if (!ImGui::BeginTabItem("Tracker")) {
        return;
    }
    d.preferencesActiveTab = PreferencesActiveTab::Tracker;
    // Pillar 2 (#892): snapshot ListCachedProjects() on the tab open-edge; the latch is reset
    // in resetPreferencesWindowState when the window closes, so reopening re-snapshots once.
    RefreshCachedProjectsSnapshotOnOpen(d, /*isOpenNow=*/true, d.prefsTrackerTabWasOpen);
    const int currentItem = DrawTrackerBackendSelection(d);
    DrawTrackerBackendConfig(d, currentItem);
    DrawTrackerRecentProjects(d, currentItem);
    if (ImGui::Button("Open Views Dashboard")) {
        d.showViewsDashboard = true;
        d.requestViewsDashboardFocus = true;
    }
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.tracker.views_note.help",
                               "Query/JQL and column fields are configured in the Views dashboard.");
    ImGui::EndTabItem();
}

void SmatchetUI::drawPreferencesUserInfoTab(UiDrawSession& d) {
    if (!ImGui::BeginTabItem("User Info")) {
        return;
    }
    d.preferencesActiveTab = PreferencesActiveTab::UserInfo;
    DrawUserInfoFeedSettings(d);
    ImGui::EndTabItem();
}

#if defined(SMATCHET_WITH_MCP)
void SmatchetUI::drawPreferencesIntegrationsTab(AppController& app, UiDrawSession& d) {
    if (!ImGui::BeginTabItem("Integrations")) {
        return;
    }
    d.preferencesActiveTab = PreferencesActiveTab::Integrations;
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
    ImGui::SetItemTooltip("Off: localhost only. On: binds 0.0.0.0.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.integrations.mcp_bind.help",
                               "When off, MCP listens on localhost only (127.0.0.1). When on, it binds "
                               "0.0.0.0 — reachable on your network. Set an auth token below if you enable "
                               "this.");
    ImGui::InputText("MCP auth token (optional)", d.mcpAuthTokenBuf, sizeof(d.mcpAuthTokenBuf),
                     ImGuiInputTextFlags_Password);
    ImGui::SetItemTooltip("Clients must send the X-Smatchet-Token header.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.integrations.mcp_token.help",
                               "If set, clients must send header X-Smatchet-Token with this value. If empty "
                               "and bind is localhost-only, only loopback clients may connect.");
    ImGui::Checkbox("Allow MCP run_lua tool (dangerous)", &d.mcpAllowLuaExecution);
    ImGui::SetItemTooltip("Lets MCP clients execute Lua. Off by default.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.integrations.mcp_lua.help",
                               "Off by default. When enabled, MCP clients can execute Lua snippets or "
                               "Scripts/*.lua via the built-in run_lua tool.");
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
    // No ProjectKey / PlaneProjectId writebacks — project is per-operation.
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
    d.cfg.LinearApiKey = d.linearApiKeyBuf;
    d.cfg.LinearBaseUrl = d.linearBaseUrlBuf;
    d.cfg.LinearTeamKey = d.linearTeamKeyBuf;
    d.cfg.LinearTeamId = d.linearTeamIdBuf;
    d.cfg.LinearWorkspaceUrl = d.linearWorkspaceUrlBuf;
    // Issue #979 — trim leading/trailing whitespace on every credential/identity field
    // BEFORE the base-URL empty-default below, so a whitespace-only buffer still gets the
    // default. A trailing space in the Jira email made Atlassian 401 every request.
    SmatchetPreferencesUiDetail::TrimTrackerCredentialFields(d.cfg);
    if (d.cfg.GitHubBaseUrl.empty()) {
        d.cfg.GitHubBaseUrl = "https://api.github.com";
    }
    if (d.cfg.LinearBaseUrl.empty()) {
        d.cfg.LinearBaseUrl = "https://api.linear.app/graphql";
    }
    ApplyInheritFieldsBuf(d.newIssueInheritFieldsBuf, d.cfg.NewIssueInheritFieldIds);
    ApplyInheritFieldsBuf(d.newIssueInheritFieldsPlaneBuf, d.cfg.NewIssueInheritFieldIdsPlane);
    ApplyInheritFieldsBuf(d.newIssueInheritFieldsGitHubBuf, d.cfg.NewIssueInheritFieldIdsGitHub);
    ApplyInheritFieldsBuf(d.newIssueInheritFieldsLinearBuf, d.cfg.NewIssueInheritFieldIdsLinear);
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
    } else if (d.cfg.TrackerType == "Linear") {
        LOG_INFO("Updated tracker config (Linear). BaseUrl='%s' TeamKey='%s' TeamId='%s' (API key length=%zu)",
                 d.cfg.LinearBaseUrl.c_str(), d.cfg.LinearTeamKey.c_str(), d.cfg.LinearTeamId.c_str(),
                 d.cfg.LinearApiKey.size());
    } else {
        LOG_INFO("Updated tracker config (Jira). Domain='%s', Email='%s'", d.cfg.Domain.c_str(),
                 smatchet::logging::pure::MaskEmailForLog(d.cfg.Email).c_str());
    }
    d.triggerCatalogRefetch = true;
    const std::string oldBackend = d.lastViewsBackendKey; // session-level (review HIGH-4)
    ViewState.EnsureLoaded(d.cfg);
    const std::string newBackend = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
    if (oldBackend != newBackend) {
        app.SetFieldCatalog({}, {}, {}, std::string());
        d.fieldCatalogWarning.clear();
        d.fieldCatalogFetchStarted = false;
        d.fieldCatalogLoading = false;
        d.lastViewsBackendKey = newBackend;
        const ViewDefinition* activeView = ViewState.GetActiveView();
        if (activeView) {
            d.cfg.JqlQuery = activeView->Jql;
            d.cfg.SelectedFields = activeView->Fields;
        }
    }
    app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
}

void SmatchetUI::drawPreferencesWindow(AppController& app, UiDrawSession& d, bool embedded) {
    // embedded (dual-ui slice 4): mobile Settings page draws the body directly into the page
    // child; skip the show-gate + beginPreferencesWindow/End chrome. Desktop path below is
    // byte-identical to the pre-slice-4 flow.
    if (!embedded) {
        if (!d.showPreferences) {
            resetPreferencesWindowState(d);
            return;
        }

        if (!beginPreferencesWindow(d)) {
            return;
        }
    }

    loadPreferencesBuffers(d);

    if (ImGui::BeginTabBar("PreferencesTabs")) {
        drawPreferencesTrackerTab(d);
        drawPreferencesUserInfoTab(d);
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
        DrawKeybindingsPreferencesTab(*this, app, d);
        DrawTemplatePreferencesTabs(*this, app, d, preferencesState_.templateFlags);
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    // Per-tab save-semantics line: only the info relevant to the active tab; the
    // (?) marker carries the full cross-tab explanation.
    const char* footerKey = "prefs.footer.tracker.short";
    const char* footerFallback = "Save & Sync writes this tab to disk and refreshes the tracker connection.";
    switch (d.preferencesActiveTab) {
    case PreferencesActiveTab::Tracker:
        break;
    case PreferencesActiveTab::Integrations:
        footerKey = "prefs.footer.integrations.short";
        footerFallback = "MCP settings save when changed. Runtime status: Automation -> Agent Bridge (MCP)...";
        break;
    case PreferencesActiveTab::Assistant:
        footerKey = "prefs.footer.assistant.short";
        footerFallback = "Assistant settings use explicit Save / Discard. Unsaved edits show an * on the tab.";
        break;
    case PreferencesActiveTab::Whisper:
    case PreferencesActiveTab::Templates:
    case PreferencesActiveTab::Keybindings:
        footerKey = "prefs.footer.autosave.short";
        footerFallback = "Settings on this tab save automatically when changed.";
        break;
    case PreferencesActiveTab::LocalData:
    case PreferencesActiveTab::Appearance:
    case PreferencesActiveTab::UserInfo:
        footerKey = "prefs.footer.immediate.short";
        footerFallback = "Options on this tab apply and save immediately.";
        break;
    case PreferencesActiveTab::Annotate:
        footerKey = "prefs.footer.annotate.short";
        footerFallback = "This tab has its own Save settings and Reload settings buttons.";
        break;
    }
    ImGui::TextWrapped("%s", SmatchetLocalization::T(footerKey, footerFallback));
    ImGui::SameLine();
    SmatchetHelpMarker::Render(
        "prefs.footer.save_sync.help",
        "Save & Sync writes the Tracker tab (and optional Integrations tab when enabled in this build) to "
        "disk and refreshes the tracker connection. Assistant, Whisper, Local data, Appearance, and template "
        "settings save automatically when changed. MCP runtime status: Automation -> Agent Bridge (MCP)... "
        "Log level and verbose logging: Inspect -> Runtime Log. The Annotate Analysis tab has its own Save "
        "settings and Reload settings buttons.");
    ImGui::Spacing();
    if (SmatchetIconLeadingButton(ICON_FA_ARROWS_ROTATE, "Save & Sync", nullptr, ImVec2(140.0f, 0.0f))) {
        onPreferencesSaveAndSync(app, d);
    }

    if (!embedded) {
        ImGui::End();
    }
}
