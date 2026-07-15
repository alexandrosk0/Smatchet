#pragma once

// Pure (ImGui-free, AppController-free) sort + field-classification helpers lifted
// out of TicketGridModel.cpp. TicketGridModel.cpp's render-plan / column-builder
// code pulls the ImGui-coupled TrackerGridFieldDisplay / TrackerDateTimeFieldEditor
// draw headers, which kept these comparators off the test denominator even though
// they touch nothing but std::string, the field metadata struct, and the already-
// tested TicketGridDurationSortPure. Splitting them here makes them bucket-A
// unit-testable; TicketGridModel.h re-includes this header so callers are unchanged.

#include "TrackerFieldSchema.h"

#include <string>

/// Total-order (-1 / 0 / +1) comparator for two raw field values under a column sort.
/// Empty values always sort last (independent of direction); "key"/"issuekey" use a
/// natural issue-key order; time-tracking fields defer to TicketGridDurationSortPure;
/// date/datetime fields compare lexically on the ISO string; number-typed and otherwise
/// numeric-looking values compare numerically; everything else is a case-insensitive
/// string compare. @p sortDirection is 1 for ascending (affects only empty placement).
int CompareFieldValuesForSort(const std::string& fieldId, const TrackerField* fieldMeta, const std::string& aVal,
                              const std::string& bVal, int sortDirection);

/// True when the column should be treated as date/datetime: a known date field id, a
/// date-typed / date-family catalog entry, or an id/name matching the date-like word
/// heuristics (date, time, created, updated, …).
bool IsTrackerDateOrDateTimeField(const std::string& fieldId, const TrackerField* field);

/// True for the attachment column ids ("attachment" / "attachments", case-insensitive).
bool IsAttachmentFieldId(const std::string& fieldId);
