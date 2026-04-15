#pragma once

#include "JiraClient.h"
#include "LocalCacheManager.h"
#include "SpreadsheetState.h"
#include <functional>
#include <string>
#include <vector>

namespace JiraDateTimeFieldEditor {

bool IsJiraDateTimePickerField(const JiraField& field);

using QueueDateTimeEditFn = std::function<void(const std::string& issueId,
                                               const JiraField& field,
                                               const std::vector<std::string>& values)>;

void RenderDateTimeFieldEditor(const CachedTicket& ticket,
                               const JiraField& field,
                               const std::string& currentValue,
                               SpreadsheetState& state,
                               const QueueDateTimeEditFn& queueEdit);

} // namespace JiraDateTimeFieldEditor
