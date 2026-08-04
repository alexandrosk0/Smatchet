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

// SMATCHET_DEVIATION(rule=duplication; reason=include overlap with sibling UI TU; owner=ui; revisit=dup-scoping)
#include "AppController.h"
#include "ConfigManager.h"
#include "EmailMaskForLog.h"
#include "IssueDraft.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerSetupPure.h"
#include "SmatchetImGuiFonts.h"
#include "FieldCatalogCache.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetTheme.h"
#include "Ui/SmatchetSecretInput.h"
#include "Ui/SmatchetIconButtons.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
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
    // Tracker "Test connection" verdict is close-scoped: reopening must not show a stale
    // "Connected" for credentials edited since. The generation bump makes any still-running
    // probe's completion a no-op (same staleness contract as the Assistant probe's cancel).
    d.trackerPrefsTestInFlight = false;
    d.trackerPrefsTestResult.clear();
    d.trackerPrefsTestResultKind = 0;
    // Same staleness contract for the verified-credential pin: a verdict that is no longer
    // on screen must not keep unlocking first-run read-only on a later Save & Sync.
    d.trackerPrefsTestVerifiedFingerprint.clear();
    ++d.trackerPrefsTestGen;
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
    d.githubProjectNumber = d.cfg.GitHubProjectNumber;
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
        SmatchetSecretInputText("API Token", d.tokenBuf, sizeof(d.tokenBuf));
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
        SmatchetSecretInputText("API Key", d.planeApiKeyBuf, sizeof(d.planeApiKeyBuf));
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
        SmatchetSecretInputText("Personal Access Token", d.githubPatBuf, sizeof(d.githubPatBuf));
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
        ImGui::InputInt("Project number", &d.githubProjectNumber);
        if (d.githubProjectNumber < 0) {
            d.githubProjectNumber = 0; // no negative board numbers; 0 keeps the feature off
        }
        ImGui::SetItemTooltip("Projects v2 board number under Owner; 0 disables project fields.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.github_project.help",
                                   "The N in github.com/orgs/<owner>/projects/N (or /users/<owner>/projects/N). "
                                   "When set, that board's custom fields appear as editable grid columns for "
                                   "issues on the board. 0 turns the feature off.");
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
        SmatchetSecretInputText("API Key", d.linearApiKeyBuf, sizeof(d.linearApiKeyBuf));
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

// Buffer -> config copy for every credential/identity field the Tracker tab edits.
// Shared by Save & Sync (writes d.cfg) and the "Test connection" probe (writes a
// throwaway copy - P2-M12). Canonicalizes TrackerType (issue #820), trims whitespace
// BEFORE the base-URL empty-defaults (issue #979: a trailing space in the Jira email
// made Atlassian 401 every request; a whitespace-only base URL still gets the default).
void CopyTrackerBuffersToConfig(const UiDrawSession& d, TrackerConfig& cfg) {
    cfg.Domain = d.domainBuf;
    cfg.Email = d.emailBuf;
    cfg.ApiToken = d.tokenBuf;
    // No ProjectKey / PlaneProjectId writebacks — project is per-operation.
    cfg.TrackerType = ConfigManager::NormalizeViewsBackendKey(std::string(d.trackerTypeBuf));
    cfg.PlaneUrl = d.planeUrlBuf;
    cfg.PlaneWorkspaceSlug = d.planeWorkspaceBuf;
    cfg.PlaneApiKey = d.planeApiKeyBuf;
    cfg.GitHubBaseUrl = d.githubBaseUrlBuf;
    cfg.GitHubPat = d.githubPatBuf;
    cfg.GitHubOwner = d.githubOwnerBuf;
    cfg.GitHubRepo = d.githubRepoBuf;
    cfg.GitHubProjectNumber = d.githubProjectNumber < 0 ? 0 : d.githubProjectNumber;
    cfg.LinearApiKey = d.linearApiKeyBuf;
    cfg.LinearBaseUrl = d.linearBaseUrlBuf;
    cfg.LinearTeamKey = d.linearTeamKeyBuf;
    cfg.LinearTeamId = d.linearTeamIdBuf;
    cfg.LinearWorkspaceUrl = d.linearWorkspaceUrlBuf;
    SmatchetPreferencesUiDetail::TrimTrackerCredentialFields(cfg);
    if (cfg.GitHubBaseUrl.empty()) {
        cfg.GitHubBaseUrl = "https://api.github.com";
    }
    if (cfg.LinearBaseUrl.empty()) {
        cfg.LinearBaseUrl = "https://api.linear.app/graphql";
    }
}

// "Test connection" probe row for the Tracker tab (P2-M12): probes the CURRENT BUFFER
// credentials (not the saved cfg) with a throwaway backend on a worker, then renders a
// verdict line - the same pattern the Assistant and Whisper tabs already use. First-run
// users can now verify a token before committing it with Save & Sync.
void DrawTrackerTestConnection(AppController& app, UiDrawSession& d) {
    if (d.trackerPrefsTestInFlight) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(SmatchetLocalization::T("prefs.tracker.test.button", "Test connection"))) {
        TrackerConfig probeCfg = d.cfg;
        CopyTrackerBuffersToConfig(d, probeCfg);
        d.trackerPrefsTestInFlight = true;
        d.trackerPrefsTestResult.clear();
        d.trackerPrefsTestResultKind = 0;
        const int probeGen = ++d.trackerPrefsTestGen;
        // Digest the exact values being probed, on the UI thread while the buffers are stable.
        // Save & Sync only clears first-run read-only when the live buffers still digest to
        // this, so probing green then editing the token cannot unlock on an unverified value.
        const std::string probeFingerprint = TrackerSetupPure::CredentialFingerprint(probeCfg);
        AppController* appPtr = &app;
        app.LaunchBackgroundTask([appPtr, probeCfg, probeGen, probeFingerprint]() {
            const TrackerReachabilityProbeResult probe = appPtr->ProbeTrackerCredentials(probeCfg);
            appPtr->PostToMainThread([probe, probeGen, probeFingerprint]() {
                if (g_ui.trackerPrefsTestGen != probeGen) {
                    return; // superseded by a newer probe or a window close — drop silently
                }
                g_ui.trackerPrefsTestInFlight = false;
                // Any non-green verdict invalidates a previously verified pin: the credentials
                // now on screen are the ones that just failed.
                g_ui.trackerPrefsTestVerifiedFingerprint.clear();
                switch (probe.Kind) {
                case TrackerReachabilityProbeKind::AuthenticatedReachable:
                    g_ui.trackerPrefsTestResultKind = 1;
                    g_ui.trackerPrefsTestVerifiedFingerprint = probeFingerprint;
                    g_ui.trackerPrefsTestResult =
                        SmatchetLocalization::T("prefs.tracker.test.ok", "Connected - credentials verified.");
                    break;
                case TrackerReachabilityProbeKind::ReachableAuthOrConfigError:
                    g_ui.trackerPrefsTestResultKind = 3;
                    g_ui.trackerPrefsTestResult =
                        std::string(SmatchetLocalization::T("prefs.tracker.test.auth",
                                                            "Reached the server, but sign-in failed: ")) +
                        probe.Diagnostic;
                    break;
                case TrackerReachabilityProbeKind::ServiceUnavailable:
                    g_ui.trackerPrefsTestResultKind = 3;
                    g_ui.trackerPrefsTestResult =
                        probe.Diagnostic.empty() ? std::string(SmatchetLocalization::T("prefs.tracker.test.unavailable",
                                                                                       "Service unavailable."))
                                                 : probe.Diagnostic;
                    break;
                case TrackerReachabilityProbeKind::TransportDown:
                default:
                    g_ui.trackerPrefsTestResultKind = 2;
                    g_ui.trackerPrefsTestResult = std::string(SmatchetLocalization::T("prefs.tracker.test.transport",
                                                                                      "Couldn't reach the server: ")) +
                                                  probe.Diagnostic;
                    break;
                }
            });
        });
    }
    if (d.trackerPrefsTestInFlight) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("prefs.tracker.test.testing", "Testing..."));
    }
    if (!d.trackerPrefsTestResult.empty()) {
        const SmatchetThemeSemanticColors& sem = SmatchetTheme::GetActiveSemanticColors();
        const ImVec4 col = d.trackerPrefsTestResultKind == 1
                               ? sem.SuccessText
                               : (d.trackerPrefsTestResultKind == 2 ? sem.WarningText : sem.ErrorText);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s", d.trackerPrefsTestResult.c_str());
        ImGui::PopStyleColor();
    }
}

// True when any field the Tracker tab edits differs between its stack buffer and the
// saved cfg value (P2-H3 — the Assistant tab's AssistantAiFieldsDiffer pattern). Drives
// the `Tracker *` dirty-tab marker and the close guard in drawPreferencesWindow. The
// base-URL compares mirror loadPreferencesBuffers' seeded defaults and the backend key
// compares normalized, so an untouched window always reads clean.
bool TrackerPrefsFieldsDiffer(const UiDrawSession& d) {
    const std::string githubBaseSaved =
        d.cfg.GitHubBaseUrl.empty() ? std::string("https://api.github.com") : d.cfg.GitHubBaseUrl;
    const std::string linearBaseSaved =
        d.cfg.LinearBaseUrl.empty() ? std::string("https://api.linear.app/graphql") : d.cfg.LinearBaseUrl;
    return d.cfg.Domain != d.domainBuf || d.cfg.Email != d.emailBuf || d.cfg.ApiToken != d.tokenBuf ||
           ConfigManager::NormalizeViewsBackendKey(std::string(d.trackerTypeBuf)) != d.cfg.TrackerType ||
           d.cfg.PlaneUrl != d.planeUrlBuf || d.cfg.PlaneWorkspaceSlug != d.planeWorkspaceBuf ||
           d.cfg.PlaneApiKey != d.planeApiKeyBuf || githubBaseSaved != d.githubBaseUrlBuf ||
           d.cfg.GitHubPat != d.githubPatBuf || d.cfg.GitHubOwner != d.githubOwnerBuf ||
           d.cfg.GitHubRepo != d.githubRepoBuf || d.cfg.GitHubProjectNumber != d.githubProjectNumber ||
           d.cfg.LinearApiKey != d.linearApiKeyBuf || linearBaseSaved != d.linearBaseUrlBuf ||
           d.cfg.LinearTeamKey != d.linearTeamKeyBuf || d.cfg.LinearTeamId != d.linearTeamIdBuf ||
           d.cfg.LinearWorkspaceUrl != d.linearWorkspaceUrlBuf;
}

// First-run explainer for the Tracker tab (dev-onboarding-first-run-quickstart, slice 2).
// Shown while TrackerSetupPure::NeedsSetup reads true — i.e. the backend has never been
// confirmed reachable, or a required credential field is still blank. The menu bar and the
// grid are already locked to this tab in that state (SmatchetUI_MainMenu.cpp), so this line
// is the only thing telling the user WHY, and what the three steps are.
void DrawTrackerFirstRunExplainer(const UiDrawSession& d) {
    if (!TrackerSetupPure::NeedsSetup(d.cfg)) {
        return;
    }
    const SmatchetThemeSemanticColors& sem = SmatchetTheme::GetActiveSemanticColors();
    ImGui::PushStyleColor(ImGuiCol_Text, sem.WarningText);
    ImGui::TextWrapped("%s", SmatchetLocalization::T("tracker.setup.title", "Finish setting up your tracker"));
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", SmatchetLocalization::T(
                                 "tracker.setup.body",
                                 "1. Pick a backend.  2. Fill in the credentials below.  3. Press Test connection. "
                                 "Save & Sync unlocks the rest of the app once the connection is verified."));
    ImGui::Separator();
}

} // namespace

void SmatchetUI::drawPreferencesTrackerTab(AppController& app, UiDrawSession& d) {
    const char* trackerTabLabel =
        TrackerPrefsFieldsDiffer(d) ? "Tracker *###TrackerPrefsTab" : "Tracker###TrackerPrefsTab";
    if (!ImGui::BeginTabItem(trackerTabLabel, nullptr, SmatchetPreferencesUiDetail::PrefsTabFlags(d, "Tracker"))) {
        return;
    }
    d.preferencesActiveTab = PreferencesActiveTab::Tracker;
    // Pillar 2 (#892): snapshot ListCachedProjects() on the tab open-edge; the latch is reset
    // in resetPreferencesWindowState when the window closes, so reopening re-snapshots once.
    RefreshCachedProjectsSnapshotOnOpen(d, /*isOpenNow=*/true, d.prefsTrackerTabWasOpen);
    DrawTrackerFirstRunExplainer(d);
    const int currentItem = DrawTrackerBackendSelection(d);
    DrawTrackerBackendConfig(d, currentItem);
    DrawTrackerTestConnection(app, d);
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
    if (!ImGui::BeginTabItem("User Info", nullptr, SmatchetPreferencesUiDetail::PrefsTabFlags(d, "User Info"))) {
        return;
    }
    d.preferencesActiveTab = PreferencesActiveTab::UserInfo;
    DrawUserInfoFeedSettings(d);
    ImGui::EndTabItem();
}

#if defined(SMATCHET_WITH_MCP)
void SmatchetUI::drawPreferencesIntegrationsTab(AppController& app, UiDrawSession& d) {
    if (!ImGui::BeginTabItem("Integrations", nullptr, SmatchetPreferencesUiDetail::PrefsTabFlags(d, "Integrations"))) {
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
    SmatchetSecretInputText("MCP auth token (optional)", d.mcpAuthTokenBuf, sizeof(d.mcpAuthTokenBuf));
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
    CopyTrackerBuffersToConfig(d, d.cfg);
    // First-run unlock: clear read-only ONLY when the credentials being saved are byte-for-byte
    // the ones a "Test connection" probe returned AuthenticatedReachable for this session. An
    // empty pin means nothing was verified (or the window was reopened), so read-only stands.
    // This is an ADDED clear — Save & Sync never touched ReadOnlyMode before; see the plan's
    // § Deviations.
    if (d.cfg.ReadOnlyMode && !d.trackerPrefsTestVerifiedFingerprint.empty() &&
        TrackerSetupPure::CredentialFingerprint(d.cfg) == d.trackerPrefsTestVerifiedFingerprint) {
        d.cfg.ReadOnlyMode = false;
        LOG_INFO("First-run setup: saved credentials match the verified probe — read-only mode cleared.");
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

namespace SmatchetPreferencesUiDetail {

int PrefsTabFlags(UiDrawSession& d, const char* canonicalName) {
    if (!d.prefsSelectTabRequest.empty() && d.prefsSelectTabRequest == canonicalName) {
        d.prefsSelectTabRequest.clear();
        return ImGuiTabItemFlags_SetSelected;
    }
    return ImGuiTabItemFlags_None;
}

} // namespace SmatchetPreferencesUiDetail

namespace {

// Settings-search index (UX critique M4): finding a setting shouldn't require knowing
// which of ~13 tabs owns it. Each entry pairs a top-level tab's canonical name with a
// lowercase keyword bag covering the labels/concepts that tab hosts. Curated by hand —
// the tab bodies are static label strings, so drift is caught on sight when a tab gains
// a new concept.
struct PrefsSearchEntry {
    const char* tab;            // canonical tab name (BeginTabItem literal) / display label
    const char* keywords;       // lowercase, space-separated
    const char* note = nullptr; // non-null: the setting lives OUTSIDE Preferences — the
                                // match renders this pointer text instead of a jump chip
                                // (P2-M10: "theme" used to send users to the wrong tab).
};

const PrefsSearchEntry kPrefsSearchIndex[] = {
    {"Tracker",
     "backend jira plane github linear connection domain email api token pat credentials workspace url project "
     "read-only sync"},
    {"User Info", "git commit repos activity feed vcs production group user changes"},
#if defined(SMATCHET_WITH_MCP)
    {"Integrations", "mcp model context protocol server port remote lan bind auth token lua automation agent"},
#endif
#if defined(SMATCHET_WITH_AI)
    {"Assistant", "ai provider model api key openai anthropic claude chat assistant temperature context"},
#endif
#if defined(SMATCHET_WITH_WHISPER)
    {"Whisper", "dictation speech voice microphone audio transcribe push to talk hotkey whisper model recording"},
#endif
    // P2-M10: bags pruned to labels that actually render on the tab; concepts that
    // live outside Preferences resolve to a pointer note instead of a wrong-tab chip.
    {"Local data", "cache database offline local data sqlite clear catalog projects"},
    {"Appearance", "font size density compact zoom ui mode mobile date format tooltip overflow vsync wheel update"},
    {"Keyboard Shortcuts", "keyboard shortcut hotkey binding chord rebind keys keybindings"},
    {"Grid", "grid rows estimate chip write badge editing markdown"},
    {"Themes", "theme color dark light contrast norton", "Themes: View > Appearance menu"},
    {"Logging", "log level verbose logging", "Log level and verbose logging: Inspect > Runtime Log"},
    // Time Estimates / Work Log Templates / Quick Comments live as sub-tabs INSIDE the
    // Fields Inputs tab — their keywords jump to that top-level parent.
    {"Fields Inputs",
     "field input default inherit issue type new issue draft autocomplete time estimate duration suggestions "
     "worklog work log template quick comment"},
    {"Annotate", "annotate perforce p4 blame changelist source"},
};

// Case-insensitive substring match of `needleLower` against haystack (also lowercased).
bool PrefsKeywordsMatch(const char* haystack, const std::string& needleLower) {
    std::string hay(haystack);
    for (std::size_t i = 0; i < hay.size(); ++i) {
        hay[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i])));
    }
    return hay.find(needleLower) != std::string::npos;
}

// The search box + match chips drawn above the tab bar. Clicking a chip selects that tab
// via d.prefsSelectTabRequest / PrefsTabFlags.
void DrawPrefsSearchBox(UiDrawSession& d) {
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##PrefsSearch", "Search settings...", d.prefsSearchBuf, sizeof(d.prefsSearchBuf));
    if (d.prefsSearchBuf[0] == '\0') {
        return;
    }
    std::string needle(d.prefsSearchBuf);
    for (std::size_t i = 0; i < needle.size(); ++i) {
        needle[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[i])));
    }
    // P2-M10: one- and two-character substrings light up half the tab set, so wait
    // for at least three characters of signal before matching.
    if (needle.size() < 3) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("prefs.search.too_short", "type at least 3 characters"));
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", SmatchetLocalization::T("prefs.search.found_in", "Found in:"));
    int matches = 0;
    for (const PrefsSearchEntry& entry : kPrefsSearchIndex) {
        if (!PrefsKeywordsMatch(entry.tab, needle) && !PrefsKeywordsMatch(entry.keywords, needle)) {
            continue;
        }
        // Wrap onto a new line when the next chip wouldn't fit — a broad query can match
        // most of the tab set and would otherwise run off the window edge. Measured from
        // the PREVIOUS item's right edge (the standard ImGui wrapping idiom): checking
        // GetContentRegionAvail before SameLine would read a fresh-line cursor and never
        // trigger.
        const char* chipText = entry.note != nullptr ? entry.note : entry.tab;
        const float chipW = ImGui::CalcTextSize(chipText).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float windowRightX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float nextChipEndX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + chipW;
        if (nextChipEndX < windowRightX) {
            ImGui::SameLine();
        }
        if (entry.note != nullptr) {
            // Lives outside Preferences — point there instead of jumping to a wrong tab.
            ImGui::TextDisabled("%s", entry.note);
        } else if (ImGui::SmallButton(entry.tab)) {
            d.prefsSelectTabRequest = entry.tab;
        }
        ++matches;
    }
    if (matches == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("prefs.search.no_match", "no matching tab"));
    }
}

} // namespace

void SmatchetUI::drawPreferencesWindow(AppController& app, UiDrawSession& d, bool embedded) {
    // embedded (dual-ui slice 4): mobile Settings page draws the body directly into the page
    // child; skip the show-gate + beginPreferencesWindow/End chrome. Desktop path below is
    // byte-identical to the pre-slice-4 flow.
    if (!embedded) {
        if (!d.showPreferences) {
            // P2-H3: the Tracker tab is buffer-staged (unlike the mostly-autosaving
            // sibling tabs), so closing with unsaved credential edits would silently
            // discard them. Reopen and route the decision through the guard modal.
            if (d.preferencesBuffersLoaded && TrackerPrefsFieldsDiffer(d)) {
                d.showPreferences = true;
                d.prefsTrackerCloseGuardOpen = true;
            } else {
                resetPreferencesWindowState(d);
                return;
            }
        }

        if (!beginPreferencesWindow(d)) {
            return;
        }
    }

    loadPreferencesBuffers(d);

    DrawPrefsSearchBox(d);

    if (ImGui::BeginTabBar("PreferencesTabs")) {
        drawPreferencesTrackerTab(app, d);
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
        DrawTemplatePreferencesTabs(*this, app.GetAvailableFields(), app, d, preferencesState_.templateFlags);
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
        // Engine-context prefill toggles for the quick-create popup — only meaningful
        // where a host engine pushes context snapshots (the Unreal-embedded build).
        DrawQuickCreatePreferencesTab(d);
#endif
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
    case PreferencesActiveTab::QuickCreate:
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

    // P2-H3 close guard: entered via the close gate above when the window was dismissed
    // with unsaved Tracker edits. Save and Discard both hand the close back to the gate
    // with the dirty state resolved (resetPreferencesWindowState drops the buffers, so
    // the gate's re-check short-circuits instead of reopening the modal forever).
    if (d.prefsTrackerCloseGuardOpen) {
        ImGui::OpenPopup("Unsaved tracker changes###PrefsTrackerCloseGuard");
        d.prefsTrackerCloseGuardOpen = false;
    }
    if (ImGui::BeginPopupModal("Unsaved tracker changes###PrefsTrackerCloseGuard", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", SmatchetLocalization::T("prefs.tracker.close_guard.body",
                                                         "The Tracker tab has unsaved edits. Save & Sync applies them; "
                                                         "closing without saving discards them."));
        ImGui::Spacing();
        if (ImGui::Button("Save & Sync")) {
            onPreferencesSaveAndSync(app, d);
            resetPreferencesWindowState(d);
            d.showPreferences = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            resetPreferencesWindowState(d);
            d.showPreferences = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!embedded) {
        ImGui::End();
    }
}
