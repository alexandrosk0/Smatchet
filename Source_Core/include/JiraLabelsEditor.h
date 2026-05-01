#pragma once

#include "LocalCacheManager.h"
#include "TrackerFieldSchema.h"
#include <functional>
#include <string>
#include <vector>

class AppController;

namespace JiraLabelsEditor {

bool IsLabelsField(const std::string& fieldId);

using QueueLabelEditFn =
    std::function<void(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values)>;

void RenderLabelsFieldEditor(AppController& app, const CachedTicket& ticket, const TrackerField& field,
                             const std::string& currentValue, const QueueLabelEditFn& queueEdit);

} // namespace JiraLabelsEditor
