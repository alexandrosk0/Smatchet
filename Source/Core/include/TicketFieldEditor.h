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
    /// `catalogProjectKey` is the project the catalog `field` came from was fetched for ("" = unscoped), resolved
    /// once per pane per frame by the caller: a components cell offers that catalog's options for its row only
    /// when the row belongs to that project. The default (unscoped) never lends them.
    static void RenderFieldCell(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                                int columnIndex, const TrackerField* field, const std::string& currentValue,
                                float availWidth, bool tooltipsEnabled, bool allowEdits, SpreadsheetState& state,
                                std::vector<PendingFieldEdit>& pendingEdits,
                                TrackerGridFieldAsyncState& trackerGridAsync, const std::string& dateFormatOption = {},
                                int thresholdDays = 0, bool singleClickToEdit = true,
                                const std::string& catalogProjectKey = std::string());

    /// Renders the floating modal used to edit ADF / long-text fields (Jira description/environment,
    /// Plane description, etc.). Must be called once per frame from a stable top-level location so the
    /// modal stays alive even if the originating cell scrolls out of view. Drains its own state when the
    /// user submits or cancels, appending any accepted edit to `pendingEdits`.
    static void RenderLongTextModal(std::vector<PendingFieldEdit>& pendingEdits);

  private:
    /// RenderFieldCell helper — dispatch the per-column RenderPlan to the matching editor (or a
    /// read-only plain-text fallback when edits are disabled). Returns true when the cell is fully
    /// handled and the caller should return; false to fall through to the time-tracking modal.
    // SMATCHET_DEVIATION(rule=duplication; reason=a member declaration and its out-of-line definition share the parameter list by necessity; owner=grid-ui; revisit=never)
    static bool DispatchEditorByPlan(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                                     const TrackerField* field, const std::string& currentValue, float availWidth,
                                     bool tooltipsEnabled, bool allowEdits, SpreadsheetState& state,
                                     std::vector<PendingFieldEdit>& pendingEdits, const std::string& dateFormatOption,
                                     int thresholdDays, bool singleClickToEdit, const std::string& catalogProjectKey);
};
