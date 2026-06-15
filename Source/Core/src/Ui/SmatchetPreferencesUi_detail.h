#pragma once

#include "ConfigManager.h"
#include "StringUtil.h"

#include <string>

// Private header for SmatchetPreferencesUi split TUs. Not installed — included only
// by SmatchetPreferencesUi.cpp and its companion _*.cpp files.

class SmatchetUI;
class AppController;
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
}

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
void DrawKeybindingsPreferencesTab(SmatchetUI& ui, AppController& app, UiDrawSession& d);
void DrawTemplatePreferencesTabs(SmatchetUI& ui, AppController& app, UiDrawSession& d,
                                 SmatchetPreferencesUiTemplateFlags& flags);
