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

void DrawGridHeaderToolbar(AppController& app, UiDrawSession& d, ViewDefinition*& activeViewForGrid,
                           const std::vector<TicketGridColumn>& columns, const std::vector<CachedTicket>& tickets,
                           bool readOnlyMode, Views& viewState, const TrackerConnectivityBannerForUi& trackerBanner);

/// Enqueue half of the grid field-edit pipeline — called once per visible PANE per
/// frame; folds the pane's freshly committed edits into the session queue
/// (latest-per-cell). No dispatch / chip decay (review MEDIUM-1 split).
void EnqueueGridFieldEdits(UiDrawSession& d, const std::vector<PendingFieldEdit>& pendingEdits, bool readOnlyMode);

/// Pump half — called ONCE per frame by the pane-window host with the FOCUSED
/// pane's live ticket snapshot: dispatches the next queued edit to a worker and
/// decays success chips.
void PumpGridFieldEdits(AppController& app, UiDrawSession& d, const std::vector<CachedTicket>& tickets,
                        bool readOnlyMode);

/// Composed enqueue+pump for single-shot callers outside the pane-window loop
/// (perf.grid_edit_pump command).
void ProcessGridFieldEdits(AppController& app, UiDrawSession& d, const std::vector<CachedTicket>& tickets,
                           const std::vector<PendingFieldEdit>& pendingEdits, bool readOnlyMode);

void MaybeToastTrackerConnectivityBanner(const AppController& app, UiDrawSession& d,
                                         const TrackerConnectivityBannerForUi& banner);

void MaybeToastGridBannerFromSession(UiDrawSession& d);
