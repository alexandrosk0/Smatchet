#pragma once

#include <memory>
#include <string>
#include <vector>

class AppController;
class IAppTicketMutations;
struct CachedTicket;
struct SpreadsheetState;
struct TrackerField;

/** True when the configured (or name-matched "callstack") tracker field has non-empty text on this row.
 *  availableFields is the app-owned field catalog (it outlives any frame; callers pass the catalog
 *  getter's result). */
bool AnnotateRowHasNonEmptyCallstackField(const std::vector<TrackerField>& availableFields, const CachedTicket& ticket);

/** Select the issue, open Annotate Analysis, and hydrate callstack from the tracker field (same as Inspect → Source
 * Annotate). availableFields is the app-owned field catalog. */
void OpenAnnotateAnalysisForGridIssue(const std::vector<TrackerField>& availableFields, bool& showAnnotateAnalysis,
                                      SpreadsheetState& gridState, const std::string& issueKey);

class AnnotateAnalysisUi {
  public:
    AnnotateAnalysisUi();
    ~AnnotateAnalysisUi();

    struct AnnotateState; // defined in AnnotateAnalysisUi.cpp; public so the module State() accessor compiles

    /** Call each frame with whether the annotate panel is shown (used to detect open/close). */
    void SetAnnotatePanelOpen(bool open);

    /** Join worker / merge results / poll detail futures (call every frame). */
    void ServiceBackground();

    void DrawWindow(AppController& app, bool* pOpen, const std::string& selectedJiraIssueKey);

    /** Draw only the inner content (no ImGui window wrapper). Use when embedding inside a tab.
     *  Sets *wantClose = true when the user clicks Close. */
    void DrawContent(AppController& app, bool* wantClose, const std::string& selectedJiraIssueKey);

    /** Persisted annotate options (same fields as former Annotate "Options…"); call from Preferences.
     *  Takes the app-owned field catalog plus the mutations facet (field-id lookup for combo previews). */
    void DrawAnnotatePreferencesTab(const std::vector<TrackerField>& availableFields,
                                    const IAppTicketMutations& ticketMutations);

  private:
    void ensureSettingsBuffersLoaded();

    std::unique_ptr<AnnotateState> state_;

    bool cfgLoaded_ = false;
    bool annotatePanelOpen_ = false;
    bool annotateOpenPrev_ = false;
};
