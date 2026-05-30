#pragma once

// Private header for SmatchetPreferencesUi split TUs. Not installed — included only
// by SmatchetPreferencesUi.cpp and its companion _*.cpp files.

class SmatchetUI;
class AppController;
struct UiDrawSession;

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
void DrawTemplatePreferencesTabs(SmatchetUI& ui, AppController& app, UiDrawSession& d,
                                 SmatchetPreferencesUiTemplateFlags& flags);
