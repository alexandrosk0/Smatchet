#pragma once

// Private header for the time-tracking (worklog) dialog split out of TicketFieldEditor.cpp. Not
// installed — included only by TicketFieldEditor.cpp and TicketFieldEditor_Worklog.cpp.

#include <string>

class IAppThreading;
class IAppTicketMutations;
struct CachedTicket;

namespace TicketFieldEditorWorklog {

/// Seeds the dialog for `ticket`, owned by the cell whose column is `ownerFieldId` (the `timespent`
/// and `worklog` cells share it, so both entry points open an identically seeded dialog).
void OpenWorklogDialog(const CachedTicket& ticket, const std::string& ownerFieldId);

/// The SpecialTimeSpent column's "Log work" / time-spent button; a click opens the dialog.
void RenderTimeSpentButton(const CachedTicket& ticket, const std::string& fieldId, const std::string& currentValue,
                           float availWidth, bool tooltipsEnabled);

/// Draws the dialog when the cell `columnFieldId` owns it (a row can show both entry cells).
void RenderTimeTrackingModal(IAppThreading& threading, IAppTicketMutations& mutations, const CachedTicket& ticket,
                             const std::string& columnFieldId);

} // namespace TicketFieldEditorWorklog
