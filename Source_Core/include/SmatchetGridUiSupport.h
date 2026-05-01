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
