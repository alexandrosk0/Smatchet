#pragma once

#include "LocalCacheManager.h"
#include "SmatchetUiSession.h"
#include "SpreadsheetState.h"
#include "TicketGridModel.h"
#include "TrackerFieldSchema.h"

#include <cstdint>
#include <string>
#include <vector>

class AppController;
class Views;

void DrawGridCellRightClickPopups(const std::string& imguiStackId, const std::string& issueKey,
                                  const std::string& fieldId, const std::string& fieldLabel,
                                  const std::string& rawValue, AppController* app, UiDrawSession* ui,
                                  bool readOnlyMode);

void DrawTicketGridHeaderContextMenu(const TicketGridColumn& col, const TrackerField* meta);

std::string GetCellRawForCopy(const CachedTicket& ticket, const TicketGridColumn& column,
                              const TrackerField* fieldMeta);

void CopyGridRectAsTsv(const std::vector<CachedTicket>& tickets, const std::vector<size_t>& sortedIdx,
                       const std::vector<TicketGridColumn>& columns, const TrackerFieldCatalogIndex& catalog,
                       const GridRectSelection& sel);

std::uint64_t ComputeGridSortSignature(const std::string& sortFingerprint, std::uint64_t ticketsRevision,
                                       std::size_t ticketCount);

std::string BuildGridContextSignature(const ViewDefinition* view, const std::string& jqlQuery);
void CancelUnfinishedNewIssueForGridChange(UiDrawSession& d);

// Consolidated Shared Utilities
bool ImGuiEffectiveKeyCtrl();
bool ImGuiEffectiveKeyShift();
std::string BuildCellKey(const std::string& issueId, const std::string& fieldId);
std::string SanitizeClipboardCell(const std::string& value);
void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory);

// --- Extracted Grid Panels & Pipelines ---

bool DrawUnifiedOfflineQueuesPanel(AppController& app, UiDrawSession& d);

void RenderNewIssueDraftRow(AppController& app, UiDrawSession& d,
                            const std::vector<TicketGridColumn>& columns,
                            const TrackerConfig& cfg,
                            const CachedTicket* lastVisibleTicket);

void DrawGridHeaderToolbar(AppController& app, UiDrawSession& d,
                           ViewDefinition*& activeViewForGrid,
                           const std::vector<TicketGridColumn>& columns,
                           const std::vector<CachedTicket>& tickets,
                           bool readOnlyMode,
                           Views& viewState,
                           const TrackerConnectivityBannerForUi& trackerBanner);

void ProcessGridFieldEdits(AppController& app, UiDrawSession& d,
                           const std::vector<CachedTicket>& tickets,
                           std::vector<PendingFieldEdit>& pendingEdits,
                           bool readOnlyMode);

void MaybeToastTrackerConnectivityBanner(AppController& app, UiDrawSession& d,
                                         const TrackerConnectivityBannerForUi& banner);

void MaybeToastGridBannerFromSession(UiDrawSession& d);







