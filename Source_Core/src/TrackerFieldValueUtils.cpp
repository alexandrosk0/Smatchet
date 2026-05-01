#include "TrackerFieldValueUtils.h"

namespace {

std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            size_t start = 0;
            size_t end = current.size();
            while (start < end && (current[start] == ' ' || current[start] == '\t')) {
                ++start;
            }
            while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t')) {
                --end;
            }
            if (end > start) {
                result.push_back(current.substr(start, end - start));
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    size_t start = 0;
    size_t end = current.size();
    while (start < end && (current[start] == ' ' || current[start] == '\t')) {
        ++start;
    }
    while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t')) {
        --end;
    }
    if (end > start) {
        result.push_back(current.substr(start, end - start));
    }
    return result;
}

} // namespace

namespace TrackerFieldValueUtils {

const TrackerFieldOption* FindOptionRecursive(const std::vector<TrackerFieldOption>& options,
                                              const std::string& value) {
    for (const auto& option : options) {
        if (option.Id == value || option.Value == value) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const TrackerFieldOption* nested = FindOptionRecursive(option.Children, value)) {
                return nested;
            }
        }
    }
    return nullptr;
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
    for (const auto& part : ParseCsv(currentValue)) {
        const std::string id = ResolveOptionId(field, part);
        if (!id.empty()) {
            ids.push_back(id);
        }
    }
    return ids;
}

const char* EmptySelectPreviewLabel(const TrackerField& field) { return field.IsUserType ? "Unassigned" : "<none>"; }

std::string BuildSelectionPreview(const TrackerField& field, const std::vector<std::string>& selectedIds) {
    if (selectedIds.empty()) {
        return field.IsUserType ? std::string("Unassigned") : std::string("<none>");
    }
    std::string preview;
    for (size_t i = 0; i < selectedIds.size(); ++i) {
        if (i != 0) {
            preview += ", ";
        }
        preview += ResolveOptionLabel(field, selectedIds[i]);
    }
    return preview;
}

std::string BuildCascadingPreview(const TrackerFieldOption& parent, const TrackerFieldOption* child) {
    if (child == nullptr) {
        return parent.Value;
    }
    return parent.Value + " > " + child->Value;
}

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
        for (const auto& child : parent.Children) {
            if (&child == option || child.Id == option->Id || child.Value == option->Value) {
                outParentId = parent.Id.empty() ? parent.Value : parent.Id;
                outChildId = child.Id.empty() ? child.Value : child.Id;
                return true;
            }
        }
    }
    outParentId = option->Id.empty() ? option->Value : option->Id;
    return true;
}

} // namespace TrackerFieldValueUtils
