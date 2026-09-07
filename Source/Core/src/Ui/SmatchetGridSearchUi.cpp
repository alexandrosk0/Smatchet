#include "SmatchetUI.h"

// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=not a new includer — this TU is SmatchetOmnibarUi.cpp renamed, and carried the same include for the same ticket-key jump (OpenUrl / BuildIssueBrowseUrl); total fan-in is unchanged, the ratchet just cannot see a rename; owner=orchestrator; revisit=when the gate detects renames, or when the jump moves behind a narrower navigation interface)
#include "AppController.h" // OpenUrl / BuildIssueBrowseUrl (ticket-key jump)
// clang-format on
#include "GridPane.h"
#include "GridSearchInputClassifier.h"
#include "SmatchetAutocompleteUi.h" // TrackerQueryAcp_QueryWithAccountIds (Jira name→accountId)
#include "SmatchetGridUiSupport.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "SmatchetLocalization.h" // T() for the toast copy (no widget in this TU)
#include "StringUtil.h"           // TrimCopyAsciiWhitespace

#include <memory>
#include <string>
#include <vector>

// Routing core for the grid header's search box. The box used to be TWO inputs: a global
// omnibox side-bar (key jump / structured query / title search, Enter-committed) plus a
// per-pane "Filter..." box whose only job the omnibox's TitleSearch branch duplicated by
// copying its text into that very buffer. The omnibox is gone; its whole input range now
// lands on the per-pane box (drawn by SmatchetGridHeaderUi.cpp), which classifies what was
// typed and routes Enter here.
//
// This TU owns the routing + the shared view-apply core only — no widget. The commit is
// LATCHED by the header and replayed by drawGridPaneWindows after the pane loop
// (PaneDeferredActionKind::GridSearchCommit), never applied mid-pane-draw: a query apply
// re-runs the view (SyncWithCurrentView) and must not fire while a pane's table is being
// submitted.

namespace gs = smatchet::gridsearch;

namespace {

// Ticket-key Enter (sync v1): jump in-grid when the row is already loaded in the pane's
// snapshot (no network), otherwise open the backend browse URL in the user's browser. Snapshot
// membership only — an async existence-fetch for not-yet-loaded keys is a deferred follow-up.
void GridSearchJumpToTicket(AppController& app, UiDrawSession& d, GridPane& target, const std::string& key) {
    const std::shared_ptr<const std::vector<CachedTicket>>& rows = target.ticketsSnapshot;
    if (rows) {
        for (const CachedTicket& ticket : *rows) {
            if (ticket.id == key) {
                target.gridState.SetActiveIssue(key);
                return;
            }
        }
    }
    app.OpenUrl(app.BuildIssueBrowseUrl(d.cfg, key));
}

} // namespace

bool GridSearchFiltersRows(const GridPane& pane) {
    // Steady state is an empty box: skip the classifier (and its two trims) entirely.
    if (pane.gridSearchBuf[0] == '\0') {
        return false;
    }
    return gs::AppliesAsRowFilter(
        gs::ClassifyGridSearchInput(pane.gridSearchBuf, gs::GridSearchBackendFromKey(pane.backendKey)));
}

std::string GridSearchRowFilterText(const GridPane& pane) {
    if (!GridSearchFiltersRows(pane)) {
        return std::string();
    }
    return std::string(pane.gridSearchBuf);
}

// Routes a committed search-box Enter against `target` per GridSearchInputClassifier. Trims
// once, then dispatches on the classified kind: TicketKey jumps the pane, TitleSearch is
// already live (the box IS the filter — the text it holds is being matched against the rows
// every frame), Jql drives applyQueryToPaneView (the shared view-apply core). Empty input is
// a no-op so Enter on a blank box never touches the view.
void SmatchetUI::applyGridSearchEnter(AppController& app, UiDrawSession& d, GridPane& target, const std::string& raw) {
    const std::string input = TrimCopyAsciiWhitespace(raw);
    if (input.empty()) {
        return;
    }
    const gs::GridSearchBackend backend = gs::GridSearchBackendFromKey(target.backendKey);
    switch (gs::ClassifyGridSearchInput(input, backend)) {
    case gs::GridSearchInputKind::TicketKey:
        GridSearchJumpToTicket(app, d, target, input);
        return;
    case gs::GridSearchInputKind::TitleSearch:
        // Nothing to apply: GridSearchRowFilterText already feeds this text to the row
        // filter every frame. Enter is just the commit gesture users expect.
        return;
    case gs::GridSearchInputKind::Jql:
    default:
        break;
    }
    // The editor buffer holds display names on Jira — apply the id-canonical form so the
    // view of record and the backend both see accountIds. Gate on the PANE's backend (the
    // search box is per-pane; d.cfg.TrackerType may describe a different pane's backend).
    // The dashboard editor supplies the search-resolved user list the reverse map needs.
    const std::string query = backend == gs::GridSearchBackend::Jira
                                  ? TrackerQueryAcp_QueryWithAccountIds(app.GetAvailableFields(),
                                                                        app.GetAvailableUsers(), d.viewJqlEditor, input)
                                  : input;
    switch (applyQueryToPaneView(app, d, target, query)) {
    case ApplyQueryResult::Ok:
        break; // happy path — the grid re-runs; no toast needed.
    case ApplyQueryResult::ViewUnavailable:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.search", "Search"),
            SmatchetLocalization::T("gridsearch.no_active_view", "No active view to search — open a grid pane first."),
            ToastType::Warning);
        break;
    case ApplyQueryResult::UpdateFailed:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.search", "Search"),
            SmatchetLocalization::T("gridsearch.apply_failed", "Could not apply the query."), ToastType::Warning);
        break;
    }
}

// Applies `query` to the view owned by `target` AS AN UNSAVED EDIT and re-runs it. Adopts
// that view's identity first (without a network re-fetch — SyncWithCurrentView below kicks
// the sync) when it isn't already the active view. Mirrors the applied query into
// d.cfg.JqlQuery and the dashboard JQL editor buffer so every surface stays in lock-step.
// Shared core of userInfoAddToQuery (User-Info "add to query") and the search box's Enter.
//
// UX critique P2-H1: this used to call ViewState.UpdateActive (which Save()s to disk
// immediately) — a throwaway search durably rewrote the saved view's query of record with
// no dirty flag and no undo. It now routes through the SAME unsaved-edit mechanism as
// column/sort edits: snapshot the pre-edit view, mutate in memory, raise the "Unsaved
// layout changes" strip (Save / Save as new... / Discard decides durability).
SmatchetUI::ApplyQueryResult SmatchetUI::applyQueryToPaneView(AppController& app, UiDrawSession& d, GridPane& target,
                                                              const std::string& query) {
    const ViewDefinition* active = ViewState.GetActiveView();
    if (active == nullptr || active->Id != target.viewId) {
        viewsActivateView(app, d, target.viewId, /*kickSync=*/false);
        active = ViewState.GetActiveView();
    }
    if (active == nullptr || active->Id != target.viewId) {
        // Activation fell back to a non-target view (or none) — never overwrite an unrelated
        // view's query.
        return ApplyQueryResult::ViewUnavailable;
    }
    ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
    if (mutableActive == nullptr || mutableActive->Id != target.viewId) {
        return ApplyQueryResult::ViewUnavailable;
    }
    // Snapshot BEFORE the mutation so the strip's Discard can restore the saved query.
    SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
    mutableActive->Jql = query;
    ViewState.BumpRevision();
    d.viewsDirty = true;
    d.cfg.JqlQuery = query;
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlEditor.buf, query);
    SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
    return ApplyQueryResult::Ok;
}
