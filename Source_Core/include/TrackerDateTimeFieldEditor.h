#pragma once

#include "LocalCacheManager.h"
#include "SpreadsheetState.h"
#include "TrackerFieldSchema.h"
#include <functional>
#include <string>
#include <vector>

namespace TrackerDateTimeFieldEditor {

bool IsTrackerDateTimePickerField(const TrackerField& field);

using QueueDateTimeEditFn =
    std::function<void(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values)>;

void RenderDateTimeFieldEditor(const CachedTicket& ticket, const TrackerField& field, const std::string& currentValue,
                               SpreadsheetState& state, const QueueDateTimeEditFn& queueEdit);

} // namespace TrackerDateTimeFieldEditor







