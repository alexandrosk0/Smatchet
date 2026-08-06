#pragma once

#include <memory>
#include <string>
#include <vector>

class AppController;
class IAppTicketMutations;
class PreferencesFilter;
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

    /** One page of the persisted annotate options. The Preferences IA scatters these across two
     *  categories (Perforce lives under Connections), so the caller picks the slice it owns and
     *  wraps it in its own PrefsSection; hydration + tracker-field autoselect run per call and are
     *  idempotent. */
    enum class AnnotatePrefsSection {
        Analysis,
        FieldMapping,
        Colors,
        Perforce,
    };

    /** Draw one persisted-annotate-options section (same fields as the former Annotate "Options…").
     *  Takes the app-owned field catalog plus the mutations facet (field-id lookup for combo previews).
     *  filter is the owning Preferences page's search filter: every row inside asks it whether to draw,
     *  and it records the ids the coverage drift-guard compares against the descriptor table. It is a
     *  reference (not a nullable pointer) precisely so no call path can skip that recording. */
    void DrawAnnotatePrefsSection(AnnotatePrefsSection section, const std::vector<TrackerField>& availableFields,
                                  const IAppTicketMutations& ticketMutations, PreferencesFilter& filter);

  private:
    void ensureSettingsBuffersLoaded();

    std::unique_ptr<AnnotateState> state_;

    bool cfgLoaded_ = false;
    bool annotatePanelOpen_ = false;
    bool annotateOpenPrev_ = false;
};
