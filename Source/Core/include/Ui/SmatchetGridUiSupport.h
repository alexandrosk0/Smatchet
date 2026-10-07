#pragma once

#include "LocalCacheManager.h"
#include "SmatchetUiSession.h"
#include "SpreadsheetState.h"
#include "TicketGridModel.h"
#include "Tracker/NewIssueInheritFields.h"
#include "TrackerFieldSchema.h"

#include <cstdint>
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

void DrawGridCellRightClickPopups(const std::string& imguiStackId, const std::string& issueKey,
                                  const std::string& fieldId, const std::string& fieldLabel,
                                  const std::string& rawValue, const std::string& richValue, AppController* app,
                                  UiDrawSession* ui, bool readOnlyMode, const CachedTicket* rowForAnnotateMenu);

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
std::string SanitizeClipboardCell(const std::string& value);
void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory);

// --- Extracted Grid Panels & Pipelines ---

bool DrawUnifiedOfflineQueuesPanel(AppController& app, UiDrawSession& d);

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

void MaybeToastTrackerConnectivityBanner(const AppController& app, UiDrawSession& d,
                                         const TrackerConnectivityBannerForUi& banner);

void MaybeToastGridBannerFromSession(UiDrawSession& d);
