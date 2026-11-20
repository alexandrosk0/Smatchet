#pragma once

#include "Sync/SyncMode.h"
#include "LocalCacheManager.h"
#include "SmatchetUiSession.h"
#include "SpreadsheetState.h"
#include "TicketGridModel.h"
#include "Tracker/NewIssueInheritFields.h"
#include "TrackerFieldSchema.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

class AppController;
class Views;

/// Begin a new-issue draft seeded from the last visible ticket (defined in
/// SmatchetGridHeaderUi.cpp). Exposed so the pane-window host can replay a DEFERRED
/// "+ New Issue" click from a not-yet-focused pane after the focus/view switch lands
/// (multi-grid Slice 3, plan item 19 — the {paneId, kind} action latch).
void StartNewIssueDraft(AppController& app, UiDrawSession& d, ViewDefinition* activeViewForGrid,
                        const std::vector<CachedTicket>& tickets);

/// What a cell's right-click menu needs: the Key cell's "Copy N selected rows as CSV" and a data cell's
/// "Set <Field> for N rows...". The first six members must be set; `field` and `paneCacheKey` only by data cells.
/// Pointers, never owned strings: per-cell grid code allocates nothing (Ui AGENTS.md). The pane builds ONE ctx
/// before its row loop; each data cell then writes only `clickedRow` and `field`.
struct GridKeyCellMenuCtx {
    const std::vector<CachedTicket>* tickets;
    const std::vector<size_t>* sortedIdx;
    const std::vector<TicketGridColumn>* columns;
    const TrackerFieldCatalogIndex* catalog;
    const GridRectSelection* sel;
    int clickedRow;
    const TrackerField* field = nullptr;       ///< the clicked column's catalog field (data cells)
    const std::string* paneCacheKey = nullptr; ///< AppController::PaneCacheBackendKey of the pane (data cells)
    /// The project `catalog` (and so `field`) was fetched for, "" = unscoped (ActiveProjectDrawCtx::catalogProjectKey)
    const std::string* catalogProjectKey = nullptr;
};

// SMATCHET_DEVIATION(rule=duplication; reason=declaration mirrors its definition's long parameter list, not a copy-paste clone; owner=grid-engine; revisit=2027-10-13)
void DrawGridCellRightClickPopups(const std::string& imguiStackId, const std::string& issueKey,
                                  const std::string& fieldId, const std::string& fieldLabel,
                                  const std::string& rawValue, const std::string& richValue, AppController* app,
                                  UiDrawSession* ui, bool readOnlyMode, const CachedTicket* rowForAnnotateMenu,
                                  const GridKeyCellMenuCtx* keyMenu = nullptr);

void DrawTicketGridHeaderContextMenu(const TicketGridColumn& col, const TrackerField* meta);

/// True when `pane`'s search box currently filters the LOADED rows: its text reads as plain title
/// words or a bare ticket key (which also jumps to the row on Enter). A structured query never
/// filters — it commits on Enter (GridSearchInputClassifier decides). Cheap: an empty box
/// short-circuits before the classifier runs. Defined in SmatchetGridSearchUi.cpp.
bool GridSearchFiltersRows(const GridPane& pane);

/// The text the row filter should match for `pane`: its trimmed search-box content when
/// GridSearchFiltersRows holds, otherwise empty (which TicketMatchesGridFilter treats as
/// "match everything"). Defined in SmatchetGridSearchUi.cpp.
std::string GridSearchRowFilterText(const GridPane& pane);

std::string GetCellRawForCopy(const CachedTicket& ticket, const TicketGridColumn& column,
                              const TrackerField* fieldMeta);

void CopyGridRectAsTsv(const std::vector<CachedTicket>& tickets, const std::vector<size_t>& sortedIdx,
                       const std::vector<TicketGridColumn>& columns, const TrackerFieldCatalogIndex& catalog,
                       const GridRectSelection& sel);

/// CSV (RFC 4180 quoting, LF line endings) of the visible grid columns for the whole-row selection `rows`: a header
/// line of column labels, then one line per row in grid sort order, values as Ctrl+C copies them.
std::string BuildGridRowsCsv(const std::vector<CachedTicket>& tickets, const std::vector<size_t>& sortedIdx,
                             const std::vector<TicketGridColumn>& columns, const TrackerFieldCatalogIndex& catalog,
                             const std::set<int>& rows);

std::uint64_t ComputeGridSortSignature(const std::string& sortFingerprint, std::uint64_t ticketsRevision,
                                       std::size_t ticketCount);

/// Select every row in `pane`'s grid (filtered set if any, else all `tickets`),
/// seeding PrimaryRow / SortSignature / ActiveIssueId. Shared by the menu-bar
/// "Select All" item and the grid-local Ctrl+A handler so the two stay in sync.
void GridSelectAllRows(GridPane& pane, const std::vector<CachedTicket>& tickets);

std::string BuildGridContextSignature(const ViewDefinition* view, const std::string& jqlQuery);
void CancelUnfinishedNewIssueForGridChange(UiDrawSession& d);
/// True when the new-issue draft row holds user-entered work worth guarding: a
/// non-whitespace summary or description, or staged attachments (P2-H4). Inherited
/// seed fields (assignee, priority, ...) alone do not count as content.
bool NewIssueDraftHasUserContent(const UiDrawSession& d);

// Consolidated Shared Utilities
bool ImGuiEffectiveKeyCtrl();
bool ImGuiEffectiveKeyShift();
std::string BuildCellKey(const std::string& issueId, const std::string& fieldId);
/// `mode` Delta (default) downloads only changed tickets; Full forces the whole-view download (Shift+Refresh).
void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory,
                         smatchet::sync::SyncMode mode = smatchet::sync::SyncMode::Delta);

// --- Extracted Grid Panels & Pipelines ---

bool DrawUnifiedOfflineQueuesPanel(AppController& app, UiDrawSession& d);
/// TEST SEAM: true while the conflict resolver shows its "your resolution is not saved" line, the answer to an
/// Esc pressed around typed text. The line is plain text with no item id, so a UI test cannot find it by label.
bool SmatchetOfflineConflictUnsavedHintShownForTest();

void RenderNewIssueDraftRow(AppController& app, UiDrawSession& d, const std::vector<TicketGridColumn>& columns,
                            const TrackerConfig& cfg, const CachedTicket* lastVisibleTicket);

/// embedded (dual-ui): the mobile Grid page draws this toolbar too, but mobile is
/// single-pane and never applies pane-add requests — the "+" / "▾" pane controls
/// are suppressed there.
void DrawGridHeaderToolbar(AppController& app, UiDrawSession& d, ViewDefinition*& activeViewForGrid,
                           const std::vector<TicketGridColumn>& columns, const std::vector<CachedTicket>& tickets,
                           bool readOnlyMode, Views& viewState, const TrackerConnectivityBannerForUi& trackerBanner,
                           bool embedded);

/// Enqueue half of the grid field-edit pipeline — called once per visible PANE per
/// frame; folds the pane's freshly committed edits into the session queue
/// (latest-per-cell). No dispatch / chip decay (review MEDIUM-1 split). Each edit should
/// carry its pane's target (AppController::LatchPendingActionTargetForPane) so it is sent
/// to, queued for and applied in that pane even if focus moves first (#2260).
void EnqueueGridFieldEdits(UiDrawSession& d, const std::vector<PendingFieldEdit>& pendingEdits, bool readOnlyMode);

/// Drop the queued, not-yet-dispatched grid edits that carry no pane target when the tracker backend
/// changes: the pump would bind those to the newly focused pane, i.e. send another backend's issue ids
/// to the new backend. An edit made in a pane keeps its own target and is kept (#2260). Sets a visible
/// error only when something was dropped.
void DiscardQueuedGridFieldEditsOnBackendSwitch(UiDrawSession& d);

/// Pump half — called ONCE per frame by the pane-window host: dispatches the next queued
/// edit to a worker, with the estimate / issue-type snapshots of the pane it was made in, and
/// decays success chips. An edit queued without a pane is bound to the focused pane here.
void PumpGridFieldEdits(AppController& app, UiDrawSession& d, bool readOnlyMode);

/// Composed enqueue+pump for single-shot callers outside the pane-window loop
/// (perf.grid_edit_pump command, UI tests); their edits go to the focused pane.
void ProcessGridFieldEdits(AppController& app, UiDrawSession& d, const std::vector<PendingFieldEdit>& pendingEdits,
                           bool readOnlyMode);

// --- Bulk field edit (docs/plans/bulk-edit-rows.md) ---

/// Pre-flight and snapshot of a bulk edit: one row per distinct id of `issueIds` with its before-value (cached
/// display value and option ids), and a Skipped state for a row that is no longer in the pane's tickets, not
/// editable (AppController::CanEditFieldForIssue, a memory read) or, when `rowValues` is given, already holding
/// the result. `rowValues` receives the values to write per row (parallel to `Rows`); null builds a preview with
/// no values. Memory only: UI thread safe.
// SMATCHET_DEVIATION(rule=duplication; reason=a header declaration mirrors its definition signature; owner=grid-engine; revisit=2027-05-15)
smatchet::grid::bulk::BulkFieldEditJob
PrepareBulkFieldEditJob(AppController& app, const std::string& paneId, const TrackerField& field,
                        const std::vector<std::string>& issueIds, const std::vector<std::string>& values,
                        smatchet::grid::bulk::BulkMode mode, std::vector<std::vector<std::string>>* rowValues);

/// Starts a bulk edit: records the job in `d.bulkFieldEditJobs`, keeps the selection by issue id, and fans the
/// rows out into `d.queuedFieldEdits` through EnqueueGridFieldEdits (every row then commits through the one edit
/// pipeline). Returns the job id, or 0 when nothing was sent (read-only, or every row skipped; a toast says why).
// SMATCHET_DEVIATION(rule=duplication; reason=a header declaration mirrors its definition signature; owner=grid-engine; revisit=2027-05-15)
std::uint64_t StartBulkFieldEdit(AppController& app, UiDrawSession& d, const std::string& paneId,
                                 const TrackerField& field, const std::vector<std::string>& issueIds,
                                 const std::vector<std::string>& values, smatchet::grid::bulk::BulkMode mode,
                                 bool readOnlyMode);

/// Folds one row's result into its job (UI thread). An unknown job is ignored with a warning. When the last row
/// lands, the one summary toast is pushed.
void FoldBulkRowResult(UiDrawSession& d, std::uint64_t jobId, const std::string& issueId,
                       smatchet::grid::bulk::BulkRowState state, const std::string& message);

/// Selects exactly the rows of `paneId`'s grid that show `issueIds` (the summary toast's "failed rows" click).
/// Applied on the pane's next draw, against its current sort order.
void SelectGridRowsByIssueIds(UiDrawSession& d, const std::string& paneId, const std::set<std::string>& issueIds);

/// Brings `d.pendingOverlay` up to date with the offline-queue snapshots (in-memory, published by the queue
/// workers: no SQLite). One shared_ptr load per snapshot per call; the index is rebuilt only when a snapshot was
/// replaced. UI thread, once per pane window per frame.
void RefreshPendingOverlay(AppController& app, UiDrawSession& d);

/// Bucket-E probe of the grid's pending-edit cue: the key cell calls `NotePendingRowCueDrawn` when it draws the
/// "pending" / "not sent" label of a row; a test reads which rows showed it since the last reset. Bounded (the first
/// 64 distinct keys), UI thread only. Compiled only under SMATCHET_BUILD_UI_TESTS (PUBLIC on ImGuiLib): a
/// production build has no probe state and the call is an empty inline.
#if defined(SMATCHET_BUILD_UI_TESTS)
void NotePendingRowCueDrawn(const std::string& issueKey, bool needsAttention);
/// 0 when the row's cue was not drawn since the last reset, 1 for "pending", 2 for "not sent".
int PendingRowCueDrawnForTests(const std::string& issueKey);
void ResetPendingRowCuesDrawnForTests();
#else
inline void NotePendingRowCueDrawn(const std::string& /*issueKey*/, bool /*needsAttention*/) {}
#endif

void MaybeToastTrackerConnectivityBanner(const AppController& app, UiDrawSession& d,
                                         const TrackerConnectivityBannerForUi& banner);

void MaybeToastGridBannerFromSession(UiDrawSession& d);
