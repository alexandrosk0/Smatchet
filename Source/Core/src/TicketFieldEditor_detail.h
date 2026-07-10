#pragma once

// Private header for TicketFieldEditor split TUs. Not installed — included only
// by TicketFieldEditor.cpp and TicketFieldEditor_Modal.cpp.

#include "TrackerFieldSchema.h"
#include <string>
#include <vector>

class IAppThreading; // fan-in Phase 6 T4: the long-text modal only launches a seed worker + posts back
struct CachedTicket;
struct SpreadsheetState;
struct PendingFieldEdit;

// Defined in TicketFieldEditor_Modal.cpp with external linkage.
void OpenLongTextEditor(IAppThreading& app, const std::string& issueId, const TrackerField& field,
                        const std::string& label, const std::string& currentStrippedValue,
                        const std::string& currentRichValue);
void CloseLongTextEditor();

#if defined(SMATCHET_BUILD_UI_TESTS)
// Test-only seam: external-linkage forwarder onto the anonymous-namespace RenderTextInlineEdit in
// TicketFieldEditor.cpp. Lets the bucket-E ImGui Test Engine test
// (tests/ui/duration_inline_edit_commit.test.cpp) drive the REAL inline-cell editor (duration path
// = DrawDurationFieldWithSuggestions, dirty-check + IsItemDeactivated + Escape glue) and observe the
// queued PendingFieldEdit. Compiled only under the ninja-ui-test build.
void SmatchetTest_RenderTextInlineEdit(const CachedTicket& ticket, const TrackerField& field, SpreadsheetState& state,
                                       std::vector<PendingFieldEdit>& pendingEdits);
#endif // SMATCHET_BUILD_UI_TESTS
