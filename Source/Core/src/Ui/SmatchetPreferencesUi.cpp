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
#include "SmatchetWindowExpand.h"
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
    // wantFocus OR layoutForceDefaultsFrames forces SetNextWindowFocus before Begin, which raises
    // the window when it is FLOATING. Post-Begin SetWindowFocus is belt-and-braces for that case.
    // A DOCKED tab needs selectDockedTab as well — SetNextWindowFocus does not touch the tab bar.
    prepareTopLevelWindow(d, "preferences", 560.0f, 480.0f, wantFocus || d.layoutForceDefaultsFrames > 0);
    if (wantFocus) {
        selectDockedTab("Preferences");
    }
    // After selectDockedTab, never before: an expanded Preferences is deliberately undocked,
    // so selectDockedTab early-returns on it, and BeginWindow's SetNextWindowDockID(0, Always)
    // must be the LAST next-window write before Begin or prepareTopLevelWindow's slot wins.
    SmatchetWindowExpand::BeginWindow(d, "Preferences");
    if (!ImGui::Begin("Preferences", &d.showPreferences)) {
        if (wantFocus) {
            d.requestPreferencesFocus = false;
        }
        ImGui::End();
        return false;
    }
    SmatchetWindowExpand::DrawToggle(d);
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

// Backend index (0=Jira, 1=Plane, 2=GitHub, 3=Linear) from the persisted type buffer.
// Defensive case-insensitive match — smatchet_config.json could be hand-edited with
// lowercase "plane"/"github"/"linear" values; the combo writer always emits canonical
// PascalCase, but the load path doesn't canonicalize. Hoisted out of
// DrawTrackerBackendSelection so the Recent-projects section resolves the backend even
// while the Backend & credentials section is collapsed (its body — and the combo's
// return value — doesn't run then).
int TrackerBackendIndexFromBuf(const UiDrawSession& d) {
    const std::string trackerTypeStr(d.trackerTypeBuf);
    if (trackerTypeStr == "Plane" || trackerTypeStr == "plane") {
        return 1;
    }
    if (trackerTypeStr == "GitHub" || trackerTypeStr == "github") {
        return 2;
    }
    if (trackerTypeStr == "Linear" || trackerTypeStr == "linear") {
        return 3;
    }
    return 0;
}

// Backend-selection section of the Tracker tab: read-only toggle + the Jira/Plane/GitHub/Linear combo.
// Returns the selected backend index (0=Jira, 1=Plane, 2=GitHub, 3=Linear). Extracted from
// drawPreferencesTrackerTab during the over-100-line decomposition; behaviour-identical.
int DrawTrackerBackendSelection(UiDrawSession& d) {
    // Heading + rules are section chrome — no descriptor rows of their own.
    if (!d.prefsFilter.Active()) {
        ImGui::TextUnformatted("Backend Selection");
        ImGui::Separator();
        ImGui::Spacing();
    }

    if (d.prefsFilter.ShowSetting("tracker.backend.read_only")) {
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
    }

    const char* items[] = {"Jira", "Plane", "GitHub", "Linear"};
    // The index is computed unconditionally: every credential row below keys off it,
    // so it must survive the backend picker being filtered out.
    int currentItem = TrackerBackendIndexFromBuf(d);
    if (!d.prefsFilter.ShowSetting("tracker.backend.type")) {
        return currentItem;
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
    if (!d.prefsFilter.Active()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
    return currentItem;
}

// One function per backend below. Every row is a ConditionalDraw descriptor: only the selected
// backend's block draws, so the drift guard tolerates the other three never being observed. The
// `chrome` flag suppresses headings/spacers while a search query is active — chrome has no
// descriptor row of its own and would otherwise survive a filter that matched nothing here.
void DrawTrackerJiraConfig(UiDrawSession& d, bool chrome) {
    if (chrome) {
        ImGui::TextUnformatted("Jira Configuration (Atlassian Cloud)");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.jira_domain")) {
        ImGui::InputText("Domain", d.domainBuf, sizeof(d.domainBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. companyname.atlassian.net");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.jira_email")) {
        ImGui::InputText("Email", d.emailBuf, sizeof(d.emailBuf));
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.jira_api_token")) {
        SmatchetSecretInputText("API Token", d.tokenBuf, sizeof(d.tokenBuf));
    }
    // No "Project Key" preference row. Project is per-operation — picked
    // via the new-issue draft picker, derived from the active view's JQL, or supplied
    // on ticket.create. The "Recently used projects" section below surfaces cached
    // projects for visibility / Forget.
    if (d.prefsFilter.ShowSetting("tracker.backend.jira_inherit")) {
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (Jira)", d.newIssueInheritFieldsBuf,
                         sizeof(d.newIssueInheritFieldsBuf));
        ImGui::SetItemTooltip("Comma-separated Jira field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_jira.help",
                                   "Comma-separated Jira field ids copied from the last grid row when you "
                                   "click + New issue (e.g. description, priority, assignee, labels, "
                                   "components).");
    }
}

void DrawTrackerPlaneConfig(UiDrawSession& d, bool chrome) {
    if (chrome) {
        ImGui::TextUnformatted("Plane Configuration (plane.so)");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.plane_url")) {
        ImGui::InputText("URL", d.planeUrlBuf, sizeof(d.planeUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. https://api.plane.so");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.plane_workspace")) {
        ImGui::InputText("Workspace Slug", d.planeWorkspaceBuf, sizeof(d.planeWorkspaceBuf),
                         ImGuiInputTextFlags_CharsNoBlank);
    }
    // No "Project ID (UUID)" preference row. See Jira note above.
    if (d.prefsFilter.ShowSetting("tracker.backend.plane_api_key")) {
        SmatchetSecretInputText("API Key", d.planeApiKeyBuf, sizeof(d.planeApiKeyBuf));
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.plane_inherit")) {
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (Plane)", d.newIssueInheritFieldsPlaneBuf,
                         sizeof(d.newIssueInheritFieldsPlaneBuf));
        ImGui::SetItemTooltip("Comma-separated Plane field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_plane.help",
                                   "Comma-separated Plane field ids copied from the last grid row when you "
                                   "click + New issue (e.g. description, priority, assignee, labels).");
    }
}

// GitHub-as-tracker — docs/plans/shipped/github-tracker-backend.md.
void DrawTrackerGitHubConfig(UiDrawSession& d, bool chrome) {
    if (chrome) {
        ImGui::TextUnformatted("GitHub Configuration (github.com or Enterprise)");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.gh_base_url")) {
        ImGui::InputText("Base URL", d.githubBaseUrlBuf, sizeof(d.githubBaseUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. https://api.github.com or https://github.your-corp.com/api/v3");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.gh_pat")) {
        SmatchetSecretInputText("Personal Access Token", d.githubPatBuf, sizeof(d.githubPatBuf));
        ImGui::SetItemTooltip("Fine-grained PAT with repo + issues + projects (read/write) scope.");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.gh_owner")) {
        ImGui::InputText("Owner", d.githubOwnerBuf, sizeof(d.githubOwnerBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("GitHub user or organization, e.g. \"alexandrosk0\".");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.gh_repo")) {
        ImGui::InputText("Repo", d.githubRepoBuf, sizeof(d.githubRepoBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Repository name, e.g. \"Smatchet\".");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.github_repo.help",
                                   "Repository name, e.g. \"Smatchet\". Combined with Owner: fetches issues "
                                   "from github.com/<owner>/<repo>. Leave both empty for cross-repo "
                                   "/search/issues.");
    }
    // Clamp outside the filter gate: the buffer is also seeded from cfg, so a negative
    // value must not survive just because the query happens to hide this row.
    if (d.githubProjectNumber < 0) {
        d.githubProjectNumber = 0; // no negative board numbers; 0 keeps the feature off
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.gh_project_number")) {
        ImGui::InputInt("Project number", &d.githubProjectNumber);
        ImGui::SetItemTooltip("Projects v2 board number under Owner; 0 disables project fields.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.github_project.help",
                                   "The N in github.com/orgs/<owner>/projects/N (or /users/<owner>/projects/N). "
                                   "When set, that board's custom fields appear as editable grid columns for "
                                   "issues on the board. 0 turns the feature off.");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.gh_inherit")) {
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (GitHub)", d.newIssueInheritFieldsGitHubBuf,
                         sizeof(d.newIssueInheritFieldsGitHubBuf));
        ImGui::SetItemTooltip("Comma-separated GitHub field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_github.help",
                                   "Comma-separated GitHub field ids copied from the last grid row when you "
                                   "click + New issue (e.g. body, labels, assignees, milestone).");
    }
}

// Linear-as-tracker — Slice 1 of docs/plans/linear-tracker-backend.md. Draft scope is
// the Team, so identity is the Team Key / Team Id pair (mirrors GitHub's Owner/Repo shape).
void DrawTrackerLinearConfig(UiDrawSession& d, bool chrome) {
    if (chrome) {
        ImGui::TextUnformatted("Linear Configuration (linear.app)");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.linear_api_key")) {
        SmatchetSecretInputText("API Key", d.linearApiKeyBuf, sizeof(d.linearApiKeyBuf));
        ImGui::SetItemTooltip("Personal API Key from Linear Settings -> API.");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.linear_team_key")) {
        ImGui::InputText("Team Key", d.linearTeamKeyBuf, sizeof(d.linearTeamKeyBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Team key, e.g. \"ENG\" (the TEAM-123 issue prefix).");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.linear_team")) {
        ImGui::InputText("Team", d.linearTeamIdBuf, sizeof(d.linearTeamIdBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Linear team UUID. Optional — resolved from Team Key when empty.");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.linear_base_url")) {
        ImGui::InputText("Base URL", d.linearBaseUrlBuf, sizeof(d.linearBaseUrlBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. https://api.linear.app/graphql");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.linear_workspace_url")) {
        ImGui::InputText("Workspace URL", d.linearWorkspaceUrlBuf, sizeof(d.linearWorkspaceUrlBuf),
                         ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("Optional workspace URL / display hint.");
    }
    if (d.prefsFilter.ShowSetting("tracker.backend.linear_inherit")) {
        ImGui::Spacing();
        ImGui::InputText("New issue: inherit fields from last row (Linear)", d.newIssueInheritFieldsLinearBuf,
                         sizeof(d.newIssueInheritFieldsLinearBuf));
        ImGui::SetItemTooltip("Comma-separated Linear field ids.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.tracker.inherit_linear.help",
                                   "Comma-separated Linear field ids copied from the last grid row when you "
                                   "click + New issue (e.g. description, priority, assignee, labels).");
    }
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
    // Plaintext-token warning is section chrome — no descriptor row of its own.
    if (!d.prefsFilter.Active()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.30f, 1.0f));
        ImGui::TextWrapped("Note: on this platform the API token is stored unencrypted in the app's private "
                           "storage. Use a scoped, revocable token.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
#endif
    const bool chrome = !d.prefsFilter.Active();
    if (currentItem == 0) {
        DrawTrackerJiraConfig(d, chrome);
    } else if (currentItem == 1) {
        DrawTrackerPlaneConfig(d, chrome);
    } else if (currentItem == 2) {
        DrawTrackerGitHubConfig(d, chrome);
    } else {
        DrawTrackerLinearConfig(d, chrome);
    }
    if (chrome) {
        ImGui::Spacing();
    }
}

// "Recently used projects" section of the Tracker tab: cached-project list filtered to the current
// backend + endpoint, each with a Forget button. Extracted from drawPreferencesTrackerTab during the
// over-100-line decomposition; behaviour-identical.
void DrawTrackerRecentProjects(UiDrawSession& d, int currentItem) {
    // "Recently used projects" — read-only listbox sourced from FieldCatalogCache,
    // filtered to the current backend + endpoint. Replaces the deleted "Project Key" /
    // "Project ID (UUID)" preference rows. Each row has a Forget button.
    // One descriptor covers the whole list: the rows are cache entries, not settings.
    if (!d.prefsFilter.ShowSetting("tracker.recent_projects.list")) {
        return;
    }
    if (!d.prefsFilter.Active()) {
        ImGui::Separator();
    }
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
    // Heading + rule are section chrome — no descriptor rows of their own.
    if (!d.prefsFilter.Active()) {
        ImGui::TextUnformatted("User Info & commit feed");
        ImGui::Separator();
        ImGui::Spacing();
    }
    // Each row below writes a session buffer; the dirty-compare at the end reads those
    // buffers, so a filtered-out row simply keeps its current value — no desync.
    if (d.prefsFilter.ShowSetting("connections.activity.git_repos")) {
        ImGui::InputText("Git commit repos", d.gitCommitReposBuf, sizeof(d.gitCommitReposBuf));
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.userinfo.git_commit_repos.help",
                                   "Comma-separated owner/repo list the GitHub commit feed queries in the User "
                                   "Info window (e.g. \"alexandrosk0/Smatchet\"). Leave empty to reuse the "
                                   "tracker's own Owner/Repo. Auth reuses the GitHub PAT.");
    }
    if (d.prefsFilter.ShowSetting("connections.activity.production_keyword")) {
        ImGui::InputText("Production group keyword", d.productionGroupKeywordBuf, sizeof(d.productionGroupKeywordBuf));
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.userinfo.production_group.help",
                                   "Case-insensitive substring marking a tracker user-group as \"production\" "
                                   "(e.g. \"prod\"). Empty disables the production-group highlight.");
    }
    if (d.prefsFilter.ShowSetting("connections.activity.day_window")) {
        ImGui::InputInt("Activity day window", &d.userActivityDayWindow);
        if (d.userActivityDayWindow < 1) {
            d.userActivityDayWindow = 1;
        }
        ImGui::SetItemTooltip("How many days back the activity feed reaches (>= 1).");
    }
    if (d.prefsFilter.ShowSetting("connections.activity.max_changes")) {
        ImGui::InputInt("Max changes per source", &d.maxUserChanges);
        if (d.maxUserChanges < 1) {
            d.maxUserChanges = 1;
        }
        ImGui::SetItemTooltip("Max submitted changes fetched per VCS source (>= 1).");
    }
    if (d.prefsFilter.ShowSetting("connections.activity.vcs_layout")) {
        const char* kLayouts[] = {"unified", "separate"};
        ImGui::Combo("VCS feed layout", &d.vcsFeedLayoutIndex, kLayouts, IM_ARRAYSIZE(kLayouts));
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.userinfo.vcs_layout.help",
                                   "unified: commits from all sources merged newest-first. separate: one "
                                   "section per VCS source.");
    }
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
    // Whole row (button + in-flight hint + verdict) is one descriptor.
    if (!d.prefsFilter.ShowSetting("tracker.backend.test_connection")) {
        return;
    }
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
    // Banner is section chrome — it owns no descriptor row, so a narrowed pane drops it.
    if (d.prefsFilter.Active() || !TrackerSetupPure::NeedsSetup(d.cfg)) {
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
    // Pillar 2 (#892): snapshot ListCachedProjects() on the page open-edge; the latch is reset
    // in resetPreferencesWindowState when the window closes, so reopening re-snapshots once.
    RefreshCachedProjectsSnapshotOnOpen(d, /*isOpenNow=*/true, d.prefsTrackerTabWasOpen);
    SmatchetPreferencesUiDetail::PrefsSection(d, "tracker.backend", [&] {
        DrawTrackerFirstRunExplainer(d);
        const int currentItem = DrawTrackerBackendSelection(d);
        DrawTrackerBackendConfig(d, currentItem);
        DrawTrackerTestConnection(app, d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "tracker.recent_projects", [&] {
        DrawTrackerRecentProjects(d, TrackerBackendIndexFromBuf(d));
        if (d.prefsFilter.ShowSetting("tracker.recent_projects.open_views_dashboard")) {
            if (ImGui::Button("Open Views Dashboard")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            ImGui::SameLine();
            SmatchetHelpMarker::Render("prefs.tracker.views_note.help",
                                       "Query/JQL and column fields are configured in the Views dashboard.");
        }
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "tracker.notifications",
                                              [&] { DrawTrackerNotificationsSectionBody(d); });
}

void SmatchetUI::drawPreferencesUserInfoTab(UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "connections.activity", [&] { DrawUserInfoFeedSettings(d); });
}

#if defined(SMATCHET_WITH_MCP)
namespace {

// MCP-server section body (immediate-save semantics — the dirty-diff block below writes
// cfg + syncs the plugin host as soon as a widget changes). Split out of the old
// Integrations tab so the PrefsSection lambda stays a one-liner.
void DrawMcpSectionBody(AppController& app, UiDrawSession& d) {
    // No title/Separator here — PrefsSection already drew the section header.
    if (d.prefsFilter.ShowSetting("connections.mcp.enabled")) {
        ImGui::Checkbox("Enable MCP server", &d.mcpEnabled);
    }
    if (d.prefsFilter.ShowSetting("connections.mcp.port")) {
        ImGui::InputInt("MCP Port", &d.mcpPort);
        if (d.mcpPort < 1) {
            d.mcpPort = 1;
        }
        if (d.mcpPort > 65535) {
            d.mcpPort = 65535;
        }
    }
    if (d.prefsFilter.ShowSetting("connections.mcp.allow_remote")) {
        ImGui::Checkbox("Bind on all interfaces (LAN)", &d.mcpAllowRemote);
        ImGui::SetItemTooltip("Off: localhost only. On: binds 0.0.0.0.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.integrations.mcp_bind.help",
                                   "When off, MCP listens on localhost only (127.0.0.1). When on, it binds "
                                   "0.0.0.0 — reachable on your network. Set an auth token below if you enable "
                                   "this.");
    }
    if (d.prefsFilter.ShowSetting("connections.mcp.auth_token")) {
        SmatchetSecretInputText("MCP auth token (optional)", d.mcpAuthTokenBuf, sizeof(d.mcpAuthTokenBuf));
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.integrations.mcp_token.help",
                                   "If set, clients must send header X-Smatchet-Token with this value. If empty "
                                   "and bind is localhost-only, only loopback clients may connect.");
    }
    if (d.prefsFilter.ShowSetting("connections.mcp.allow_lua")) {
        ImGui::Checkbox("Allow MCP run_lua tool (dangerous)", &d.mcpAllowLuaExecution);
        ImGui::SetItemTooltip("Lets MCP clients execute Lua. Off by default.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.integrations.mcp_lua.help",
                                   "Off by default. When enabled, MCP clients can execute Lua snippets or "
                                   "Scripts/*.lua via the built-in run_lua tool.");
    }
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
    // Saved-hint + the two trailing notes are status chrome, not settings.
    if (d.prefsFilter.Active()) {
        return;
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
}

} // namespace

void SmatchetUI::drawPreferencesIntegrationsTab(AppController& app, UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "connections.mcp", [&] { DrawMcpSectionBody(app, d); });
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
    // First-run unlock: clear read-only AND latch reachability ONLY when the credentials being
    // saved are byte-for-byte the ones a "Test connection" probe returned AuthenticatedReachable
    // for this session. An empty pin means nothing was verified (or the window was reopened), so
    // read-only stands. The two flags move as a pair inside the pure helper so bucket-A covers
    // the pairing. The helper also CONSUMES the pin on the firing edge (hence the non-const ref),
    // so a read-only mode the user re-enables later in this same Preferences session survives the
    // next save instead of being reverted by the stale verdict. This is an ADDED clear — Save &
    // Sync never touched ReadOnlyMode before; see the plan's § Deviations.
    if (TrackerSetupPure::ApplyVerifiedSaveUnlock(d.cfg, d.trackerPrefsTestVerifiedFingerprint)) {
        LOG_INFO("First-run setup: saved credentials match the verified probe — read-only mode cleared, "
                 "backend reachability latched.");
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

namespace {

// The global settings search box, drawn above the nav rail. The query drives
// PreferencesFilter, which hides non-matching widgets in place across every
// category — no jump chips, no minimum query length: the descriptor table is the
// index, so a one-character query narrows rather than lighting up whole tabs.
// Split in two around the caller's Update() + dynamic-match fold: the input half
// only edits the buffer, the readout half reports the resulting MatchCount().
// Nothing between them emits ImGui, so the readout's SameLine still attaches to
// the clear button.
void DrawPrefsSearchBoxInput(UiDrawSession& d) {
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##PrefsSearch", "Search settings...", d.prefsSearchBuf, sizeof(d.prefsSearchBuf));
    const bool hasQuery = d.prefsSearchBuf[0] != '\0';
    if (hasQuery) {
        // Clear button, drawn before the caller's Update() so the cleared buffer
        // takes effect this same frame rather than leaving one stale filtered
        // frame behind.
        ImGui::SameLine();
        if (ImGui::SmallButton("x###PrefsSearchClear")) {
            d.prefsSearchBuf[0] = '\0';
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", SmatchetLocalization::T("prefs.search.clear", "Clear search"));
        }
    }
}

// The "showing N / M" readout. Drawn after the caller folded in the dynamic
// (descriptor-less) matches, so a query that only hits a keybinding row reports
// a count instead of "No settings match."
void DrawPrefsSearchReadout(UiDrawSession& d) {
    if (!d.prefsFilter.Active()) {
        return;
    }
    ImGui::SameLine();
    const std::size_t matches = d.prefsFilter.MatchCount();
    if (matches == 0) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("prefs.search.no_match", "No settings match."));
        return;
    }
    // Built with snprintf rather than passed as a format string: the localized
    // TextDisabled routes its fmt argument through TranslateSourceAsFormat, so a
    // translated fragment must never carry the %d placeholders itself.
    char readout[96];
    std::snprintf(readout, sizeof(readout), "%s %d / %d", SmatchetLocalization::T("prefs.search.showing", "showing"),
                  static_cast<int>(matches), static_cast<int>(d.prefsFilter.TotalCount()));
    ImGui::TextDisabled("%s", readout);
}

} // namespace

/// The right-pane body for the selected category. Runs inside the caller's
/// "PrefsBody" child; feature-gated categories draw nothing in a feature-OFF
/// build (the nav rail never offers them, so this is a stale-selection guard).
void SmatchetUI::drawPreferencesCategoryBody(AppController& app, UiDrawSession& d) {
    switch (d.preferencesCategory) {
    case PreferencesCategory::General:
        DrawGeneralPreferencesTab(*this, app, d);
        break;
    case PreferencesCategory::Appearance:
        DrawAppearancePreferencesTab(d);
        break;
    case PreferencesCategory::Tracker:
        drawPreferencesTrackerTab(app, d);
        break;
    case PreferencesCategory::Connections:
        drawPreferencesConnectionsTab(app, d);
        break;
    case PreferencesCategory::AiVoice:
        // Two features, one category: each half compiles out with its own flag, and the
        // nav rail hides the category only when both are off.
#if defined(SMATCHET_WITH_AI)
        DrawAssistantPreferencesTab(app, d);
#endif
#if defined(SMATCHET_WITH_WHISPER)
        DrawWhisperPreferencesTab(app, d);
#endif
        break;
    case PreferencesCategory::Editing:
        DrawEditingPreferencesTab(d, preferencesState_.templateFlags);
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
        // Engine-context prefill toggles for the quick-create popup — only meaningful
        // where a host engine pushes context snapshots (the Unreal-embedded build).
        DrawQuickCreatePreferencesTab(d);
#endif
        break;
    case PreferencesCategory::Shortcuts:
        DrawKeybindingsPreferencesTab(*this, app, d);
        break;
    case PreferencesCategory::Annotate:
        drawPreferencesAnnotateTab(app, d);
        break;
    }
}

/// Connections: MCP server, the Perforce executables/commands that used to sit in the
/// Annotate tab, and the activity-feed sources from the old User Info tab.
void SmatchetUI::drawPreferencesConnectionsTab(AppController& app, UiDrawSession& d) {
#if defined(SMATCHET_WITH_MCP)
    drawPreferencesIntegrationsTab(app, d);
#endif
    SmatchetPreferencesUiDetail::PrefsSection(d, "connections.perforce", [&] {
        DrawAnnotatePrefsSectionForwarded(AnnotateAnalysisUi::AnnotatePrefsSection::Perforce, app.GetAvailableFields(),
                                          app, d.prefsFilter);
    });
    drawPreferencesUserInfoTab(d);
}

/// Annotate: what stayed behind after the Perforce block moved to Connections.
void SmatchetUI::drawPreferencesAnnotateTab(AppController& app, UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "annotate.analysis", [&] {
        DrawAnnotatePrefsSectionForwarded(AnnotateAnalysisUi::AnnotatePrefsSection::Analysis, app.GetAvailableFields(),
                                          app, d.prefsFilter);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "annotate.field_mapping", [&] {
        DrawAnnotatePrefsSectionForwarded(AnnotateAnalysisUi::AnnotatePrefsSection::FieldMapping,
                                          app.GetAvailableFields(), app, d.prefsFilter);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "annotate.colors", [&] {
        DrawAnnotatePrefsSectionForwarded(AnnotateAnalysisUi::AnnotatePrefsSection::Colors, app.GetAvailableFields(),
                                          app, d.prefsFilter);
    });
}

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

    DrawPrefsSearchBoxInput(d);
    d.prefsFilter.Update(d.prefsSearchBuf, d.cfg.UiLanguage);

    // The keybindings rows are dynamic and carry no descriptors, so the schema-driven
    // filter cannot see a command-name query. Fold the answer back in here — after
    // Update(), before anything reads the result — so the match readout, the rail's
    // enabled state, the auto-switch target and the section's own visibility all agree
    // on the same frame.
    d.prefsKeybindRowsMatchQuery = SmatchetPreferencesUiDetail::AnyKeybindingRowMatchesQuery(app, d);
    if (d.prefsKeybindRowsMatchQuery) {
        d.prefsFilter.AddDynamicMatch("shortcuts.keyboard.bindings");
    }
    DrawPrefsSearchReadout(d);

    // Dirty markers for the nav labels (the old dirty-tab "*"). Computed here because
    // TrackerPrefsFieldsDiffer lives in this TU's anonymous namespace and the assistant
    // diff helper is AI-gated.
    const bool trackerDirty = d.preferencesBuffersLoaded && TrackerPrefsFieldsDiffer(d);
    bool assistantDirty = false;
#if defined(SMATCHET_WITH_AI)
    assistantDirty = d.assistantPrefsWorkingSeeded &&
                     SmatchetPreferencesUiDetail::AssistantAiFieldsDiffer(d.assistantPrefsWorking, d.cfg);
#endif

    // Reserve the footer strip (separator + Save & Sync row) so the nav rail and the
    // right pane share the remaining height.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const float bodyHeight = ImGui::GetContentRegionAvail().y - footerHeight;
    d.prefsNavCombo = SmatchetPreferencesUiDetail::ResolvePrefsNavUseCombo(embedded, ImGui::GetContentRegionAvail().x,
                                                                           ImGui::GetFontSize(), d.prefsNavCombo);
    SmatchetPreferencesUiDetail::DrawPrefsNav(d, trackerDirty, assistantDirty, bodyHeight);
    float paneHeight = bodyHeight;
    if (d.prefsNavCombo) {
        // The combo consumed a row above the pane; re-measure what's left.
        paneHeight = ImGui::GetContentRegionAvail().y - footerHeight;
    } else {
        ImGui::SameLine();
    }
    if (ImGui::BeginChild("PrefsBody", ImVec2(0.0f, paneHeight))) {
        drawPreferencesCategoryBody(app, d);
    }
    ImGui::EndChild();
    if (d.preferencesCategory != PreferencesCategory::Shortcuts) {
        // Preserve the old inactive-BeginTabItem early-return semantics: an armed
        // hotkey capture dies when the page stops drawing.
        SmatchetPreferencesUiDetail::ResetKeybindingsCaptureState();
    }

    ImGui::Spacing();
    ImGui::Separator();
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
