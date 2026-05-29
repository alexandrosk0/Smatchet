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
                                int columnIndex, const TrackerField* field, const std::string& currentValue,
                                float availWidth, bool tooltipsEnabled, bool allowEdits, SpreadsheetState& state,
                                std::vector<PendingFieldEdit>& pendingEdits,
                                TrackerGridFieldAsyncState& trackerGridAsync, const std::string& dateFormatOption = {},
                                int thresholdDays = 0, bool singleClickToEdit = true);

    /// Renders the floating modal used to edit ADF / long-text fields (Jira description/environment,
    /// Plane description, etc.). Must be called once per frame from a stable top-level location so the
    /// modal stays alive even if the originating cell scrolls out of view. Drains its own state when the
    /// user submits or cancels, appending any accepted edit to `pendingEdits`.
    static void RenderLongTextModal(std::vector<PendingFieldEdit>& pendingEdits);
};
