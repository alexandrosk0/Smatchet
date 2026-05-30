#pragma once

// Private header for TicketFieldEditor split TUs. Not installed — included only
// by TicketFieldEditor.cpp and TicketFieldEditor_Modal.cpp.

#include "TrackerFieldSchema.h"
#include <string>

class AppController;

// Defined in TicketFieldEditor_Modal.cpp with external linkage.
void OpenLongTextEditor(AppController& app, const std::string& issueId, const TrackerField& field,
                        const std::string& label, const std::string& currentStrippedValue,
                        const std::string& currentRichValue);
void CloseLongTextEditor();
