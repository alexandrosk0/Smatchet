#include "TrackerFieldValueUtils.h"
#include "StringUtil.h"
#include "ConfigManager.h"
#include <fstream>
#include <algorithm>

namespace {
} // namespace

namespace TrackerFieldValueUtils {

namespace {

const TrackerFieldOption* FindOptionByIdRecursive(const std::vector<TrackerFieldOption>& options,
                                                  const std::string& value) {
    for (const auto& option : options) {
        if (option.Id == value) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const TrackerFieldOption* nested = FindOptionByIdRecursive(option.Children, value)) {
                return nested;
            }
        }
    }
    return nullptr;
}

const TrackerFieldOption* FindOptionByValueRecursive(const std::vector<TrackerFieldOption>& options,
                                                     const std::string& value) {
    for (const auto& option : options) {
        if (option.Value == value) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const TrackerFieldOption* nested = FindOptionByValueRecursive(option.Children, value)) {
                return nested;
            }
        }
    }
    return nullptr;
}

} // namespace

// Id-preferred two-pass: match option.Id across all options first, only then fall back to
// option.Value. Keeps editor selection-id resolution consistent with ResolveOptionLabel and
// avoids the id/name collision (a component named "10033" shadowing the option whose Id is 10033).
const TrackerFieldOption* FindOptionRecursive(const std::vector<TrackerFieldOption>& options,
                                              const std::string& value) {
    if (const TrackerFieldOption* byId = FindOptionByIdRecursive(options, value)) {
        return byId;
    }
    return FindOptionByValueRecursive(options, value);
}

std::string ResolveOptionId(const TrackerField& field, const std::string& value) {
    if (const TrackerFieldOption* option = FindOptionRecursive(field.AllowedValueOptions, value)) {
        return option->Id;
    }
    return value;
}

std::string ResolveOptionLabel(const TrackerField& field, const std::string& value) {
    if (const TrackerFieldOption* option = FindOptionRecursive(field.AllowedValueOptions, value)) {
        return option->Value;
    }
    return value;
}

std::vector<std::string> ResolveCurrentSelectionIds(const TrackerField& field, const std::string& currentValue) {
    std::vector<std::string> ids;
    for (const auto& part : SplitAndTrim(currentValue)) {
        const std::string id = ResolveOptionId(field, part);
        if (!id.empty()) {
            ids.push_back(id);
        }
    }
    return ids;
}

const char* EmptySelectPreviewLabel(const TrackerField& field) { return field.IsUserType ? "Unassigned" : "<none>"; }

bool TryResolveCascadingSelection(const TrackerField& field, const std::string& currentValue, std::string& outParentId,
                                  std::string& outChildId) {
    outParentId.clear();
    outChildId.clear();
    if (currentValue.empty()) {
        return false;
    }
    const size_t sep = currentValue.find('\x1f');
    if (sep != std::string::npos) {
        outParentId = currentValue.substr(0, sep);
        outChildId = currentValue.substr(sep + 1);
        return !outParentId.empty();
    }
    const TrackerFieldOption* option = FindOptionRecursive(field.AllowedValueOptions, currentValue);
    if (option == nullptr) {
        outParentId = currentValue;
        return true;
    }
    for (const auto& parent : field.AllowedValueOptions) {
        if (&parent == option || parent.Id == option->Id || parent.Value == option->Value) {
            outParentId = parent.Id.empty() ? parent.Value : parent.Id;
            return true;
        }
        auto childIt = std::find_if(parent.Children.begin(), parent.Children.end(), [&](const auto& child) {
            return &child == option || child.Id == option->Id || child.Value == option->Value;
        });
        if (childIt != parent.Children.end()) {
            outParentId = parent.Id.empty() ? parent.Value : parent.Id;
            outChildId = childIt->Id.empty() ? childIt->Value : childIt->Id;
            return true;
        }
    }
    outParentId = option->Id.empty() ? option->Value : option->Id;
    return true;
}

std::vector<std::string> LoadDurationSuggestions() {
    return ConfigManager::Load().DurationSuggestions;
}

std::vector<std::string> LoadCommentTemplates() {
    return ConfigManager::Load().WorkLogCommentTemplates;
}

bool IsTimeDurationField(const std::string& fieldId) {
    return fieldId == "timeoriginalestimate" ||
           fieldId == "timeestimate" ||
           fieldId == "timespent" ||
           fieldId == "aggregatetimeoriginalestimate" ||
           fieldId == "aggregatetimeestimate" ||
           fieldId == "aggregatetimespent";
}

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return fieldId == "timeoriginalestimate" || fieldId == "timeestimate";
}

bool IsNonEditableTimetrackingFieldId(const std::string& fieldId) {
    return fieldId == "timespent" || fieldId == "aggregatetimeoriginalestimate" || fieldId == "aggregatetimeestimate" ||
           fieldId == "aggregatetimespent";
}

} // namespace TrackerFieldValueUtils






