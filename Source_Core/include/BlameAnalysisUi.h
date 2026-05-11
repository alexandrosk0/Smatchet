#pragma once

#include <memory>
#include <string>

class AppController;
struct CachedTicket;
struct SpreadsheetState;

/** True when the configured (or name-matched "callstack") tracker field has non-empty text on this row. */
bool BlameRowHasNonEmptyCallstackField(const AppController& app, const CachedTicket& ticket);

/** Select the issue, open Blame Analysis, and hydrate callstack from the tracker field (same as Inspect → Source Blame). */
void OpenBlameAnalysisForGridIssue(AppController& app, bool& showBlameAnalysis, SpreadsheetState& gridState,
                                   const std::string& issueKey);

class BlameAnalysisUi {
  public:
    BlameAnalysisUi();
    ~BlameAnalysisUi();

    struct BlameState; // defined in BlameAnalysisUi.cpp; public so the module State() accessor compiles

    /** Call each frame with whether the blame panel is shown (used to detect open/close). */
    void SetBlamePanelOpen(bool open);

    /** Join worker / merge results / poll detail futures (call every frame). */
    void ServiceBackground();

    void DrawWindow(AppController& app, bool* pOpen, const std::string& selectedJiraIssueKey);

    /** Persisted blame options (same fields as former Blame "Options…"); call from Preferences. */
    void DrawBlamePreferencesTab(const AppController& app);

  private:
    void ensureSettingsBuffersLoaded();

    std::unique_ptr<BlameState> state_;

    bool cfgLoaded_ = false;
    bool blamePanelOpen_ = false;
    bool blameOpenPrev_ = false;
};






