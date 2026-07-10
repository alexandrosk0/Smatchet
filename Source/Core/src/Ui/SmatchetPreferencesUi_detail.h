#pragma once

#include "ConfigManager.h"
#include "StringUtil.h"

#include <string>

// Private header for SmatchetPreferencesUi split TUs. Not installed — included only
// by SmatchetPreferencesUi.cpp and its companion _*.cpp files.

class SmatchetUI;
class AppController;
class IAppCommands; // fan-in Phase 6: keybindings tab takes the narrow registry facet
struct UiDrawSession;

namespace SmatchetPreferencesUiDetail {

/// Maps the persisted cfg.DateFormatOption string to the Combo index used by the
/// Appearance tab's "Date Format Style" dropdown. Unknown / "compact" → 0. Pure —
/// no ImGui / no session state — so it is bucket-A testable in isolation.
inline int DateFormatOptionToIndex(const std::string& option) {
    if (option == "always_relative") {
        return 1;
    }
    if (option == "absolute_iso") {
        return 2;
    }
    if (option == "absolute_friendly") {
        return 3;
    }
    return 0;
}

/// Inverse of DateFormatOptionToIndex: maps a Combo index back to the persisted
/// cfg.DateFormatOption string. Out-of-range / 0 → "compact". Pure.
inline std::string DateFormatIndexToOption(int index) {
    switch (index) {
    case 1:
        return "always_relative";
    case 2:
        return "absolute_iso";
    case 3:
        return "absolute_friendly";
    default:
        return "compact";
    }
}

/// Issue #979 — trim leading/trailing ASCII whitespace from every credential /
/// identity field `onPreferencesSaveAndSync` writes back from the text buffers.
/// A trailing space in the Jira email (observed on a real user config) makes
/// Atlassian reject basic auth with a persistent 401; the same failure class
/// applies to tokens, PATs, URLs, and owner/repo. Pure — no ImGui / no session
/// state — so it is bucket-A testable in isolation.
inline void TrimTrackerCredentialFields(TrackerConfig& cfg) {
    cfg.Domain = TrimCopyAsciiWhitespace(cfg.Domain);
    cfg.Email = TrimCopyAsciiWhitespace(cfg.Email);
    cfg.ApiToken = TrimCopyAsciiWhitespace(cfg.ApiToken);
    cfg.PlaneUrl = TrimCopyAsciiWhitespace(cfg.PlaneUrl);
    cfg.PlaneWorkspaceSlug = TrimCopyAsciiWhitespace(cfg.PlaneWorkspaceSlug);
    cfg.PlaneApiKey = TrimCopyAsciiWhitespace(cfg.PlaneApiKey);
    cfg.GitHubBaseUrl = TrimCopyAsciiWhitespace(cfg.GitHubBaseUrl);
    cfg.GitHubPat = TrimCopyAsciiWhitespace(cfg.GitHubPat);
    cfg.GitHubOwner = TrimCopyAsciiWhitespace(cfg.GitHubOwner);
    cfg.GitHubRepo = TrimCopyAsciiWhitespace(cfg.GitHubRepo);
    cfg.LinearApiKey = TrimCopyAsciiWhitespace(cfg.LinearApiKey);
    cfg.LinearBaseUrl = TrimCopyAsciiWhitespace(cfg.LinearBaseUrl);
    cfg.LinearTeamKey = TrimCopyAsciiWhitespace(cfg.LinearTeamKey);
    cfg.LinearTeamId = TrimCopyAsciiWhitespace(cfg.LinearTeamId);
    cfg.LinearWorkspaceUrl = TrimCopyAsciiWhitespace(cfg.LinearWorkspaceUrl);
}

#if defined(SMATCHET_WITH_AI)
/// The AI-related `TrackerConfig` fields the Assistant Preferences tab edits.
/// Copies just these from `src` into `dst`, leaving every non-AI field of `dst`
/// untouched. Used to (a) seed the tab's working copy from `cfg` on open /
/// Discard and (b) commit the working copy back into `cfg` on Save. Pure — no
/// ImGui / no session state — so it is bucket-E/-A testable in isolation.
inline void CopyAssistantAiFields(const TrackerConfig& src, TrackerConfig& dst) {
    dst.AiProviderKind = src.AiProviderKind;
    dst.AiApiKey = src.AiApiKey;
    dst.AiAnthropicApiKey = src.AiAnthropicApiKey;
    dst.AiDeepSeekApiKey = src.AiDeepSeekApiKey;
    dst.AiBaseUrl = src.AiBaseUrl;
    dst.AiOllamaBaseUrl = src.AiOllamaBaseUrl;
    dst.AiDeepSeekBaseUrl = src.AiDeepSeekBaseUrl;
    dst.AiModelOpenAi = src.AiModelOpenAi;
    dst.AiModelAnthropic = src.AiModelAnthropic;
    dst.AiModelOllama = src.AiModelOllama;
    dst.AiModelDeepSeek = src.AiModelDeepSeek;
    dst.AiReasoningEffort = src.AiReasoningEffort;
    dst.AiAllowCustomEndpointOpenAi = src.AiAllowCustomEndpointOpenAi;
    dst.AiAllowCustomEndpointAnthropic = src.AiAllowCustomEndpointAnthropic;
    dst.AgentsMdGlobalPath = src.AgentsMdGlobalPath;
    dst.ProjectAgentsMdPath = src.ProjectAgentsMdPath;
    dst.AgentsMdAutoDiscoverProject = src.AgentsMdAutoDiscoverProject;
}

/// True when any AI field the Assistant tab edits differs between `a` and `b`.
/// Drives the `Assistant *` dirty-tab marker + the Save/Discard enable state.
/// Pure — bucket-E/-A testable in isolation.
inline bool AssistantAiFieldsDiffer(const TrackerConfig& a, const TrackerConfig& b) {
    return a.AiProviderKind != b.AiProviderKind || a.AiApiKey != b.AiApiKey ||
           a.AiAnthropicApiKey != b.AiAnthropicApiKey || a.AiDeepSeekApiKey != b.AiDeepSeekApiKey ||
           a.AiBaseUrl != b.AiBaseUrl || a.AiOllamaBaseUrl != b.AiOllamaBaseUrl ||
           a.AiDeepSeekBaseUrl != b.AiDeepSeekBaseUrl || a.AiModelOpenAi != b.AiModelOpenAi ||
           a.AiModelAnthropic != b.AiModelAnthropic || a.AiModelOllama != b.AiModelOllama ||
           a.AiModelDeepSeek != b.AiModelDeepSeek || a.AiReasoningEffort != b.AiReasoningEffort ||
           a.AiAllowCustomEndpointOpenAi != b.AiAllowCustomEndpointOpenAi ||
           a.AiAllowCustomEndpointAnthropic != b.AiAllowCustomEndpointAnthropic ||
           a.AgentsMdGlobalPath != b.AgentsMdGlobalPath || a.ProjectAgentsMdPath != b.ProjectAgentsMdPath ||
           a.AgentsMdAutoDiscoverProject != b.AgentsMdAutoDiscoverProject;
}

/// #1706 — reset the Assistant-tab seed latches when the Preferences window
/// closes (Discard-on-close). Clearing `workingSeeded` makes the working COPY
/// re-seed from cfg on reopen; setting `forceReseed` makes SeedAssistantBuffers
/// overwrite the function-static InputText buffers (s_bufs) from that reverted
/// copy — WITHOUT it, s_bufs.aiBufsSeeded/agentsBufsSeeded persist across close
/// and the fields keep displaying the discarded edit text. Both latches must move
/// together (the explicit Discard button — DiscardAssistantWorking — sets the
/// same `forceReseed`); operates on the two bools so it stays pure + bucket-A
/// testable in isolation (no UiDrawSession dependency).
inline void ResetAssistantPrefsSeedLatchesOnClose(bool& workingSeeded, bool& forceReseed) {
    workingSeeded = false;
    forceReseed = true;
}
#endif // SMATCHET_WITH_AI

} // namespace SmatchetPreferencesUiDetail

// Lazy-load flags for template lists in DrawTemplatePreferencesTabs.
// Declared here so drawPreferencesWindow can reset them on window close.
struct SmatchetPreferencesUiTemplateFlags {
    bool suggestionsLoaded = false;
    bool templatesLoaded = false;
    bool quickTemplatesLoaded = false;
    bool annotateTemplatesLoaded = false;
};

#if defined(SMATCHET_WITH_AI)
void DrawAssistantPreferencesTab(AppController& app, UiDrawSession& d);
#endif

#if defined(SMATCHET_WITH_WHISPER)
void DrawWhisperPreferencesTab(AppController& app, UiDrawSession& d);
#endif

void DrawLocalAndAppearancePreferencesTabs(SmatchetUI& ui, AppController& app, UiDrawSession& d);
void DrawKeybindingsPreferencesTab(SmatchetUI& ui, IAppCommands& app, UiDrawSession& d);
void DrawTemplatePreferencesTabs(SmatchetUI& ui, AppController& app, UiDrawSession& d,
                                 SmatchetPreferencesUiTemplateFlags& flags);
