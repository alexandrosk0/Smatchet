#pragma once

#include "LocalCacheManager.h"
#include "SmatchetUiSession.h"
#include "TicketGridModel.h"
#include "TrackerFieldSchema.h"

#include <string>
#include <vector>

class AppController;
struct TrackerGridFieldAsyncState;

class TicketFieldEditor {
  public:
    static void RenderFieldCell(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                                const TrackerField* field, const std::string& currentValue, float availWidth,
                                bool tooltipsEnabled, bool allowEdits, SpreadsheetState& state,
                                std::vector<PendingFieldEdit>& pendingEdits, TrackerGridFieldAsyncState& trackerGridAsync);
};






