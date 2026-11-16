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
/// Keyboard: the caret starts in Time spent; Enter there, or Ctrl+Enter anywhere in the dialog, saves
/// like the Save button; Esc cancels (leaving a field that has typed text first).
void RenderTimeTrackingModal(IAppThreading& threading, IAppTicketMutations& mutations, const CachedTicket& ticket,
                             const std::string& columnFieldId);

#if defined(SMATCHET_BUILD_UI_TESTS)
/// Test hook: a copy of the dialog's state as of its last draw. UI thread only.
struct WorklogDialogTestSnapshot {
    bool Active = false;
    bool SubmitInFlight = false;
    /// The caret is in the Time spent field.
    bool TimeSpentActive = false;
    std::string TimeSpent;
    std::string Description;
    std::string ErrorMsg;
};
WorklogDialogTestSnapshot GetWorklogDialogSnapshotForTests();
/// Test hook: the dialog closes itself on its next draw (the only place a popup can be closed), the
/// way a finished save does. A test that only cleared the state would leave the popup open.
void RequestCloseWorklogDialogForTests();
#endif // SMATCHET_BUILD_UI_TESTS

} // namespace TicketFieldEditorWorklog
