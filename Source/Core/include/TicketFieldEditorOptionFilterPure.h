#pragma once

#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include <string>

// Pure option-filter predicate lifted from TicketFieldEditor.cpp's select / multi-select
// dropdown bodies. The single/multi/cascading editors all filtered options by matching a
// lower-cased search term against the option's Value, SecondaryValue, and effective id
// (Id, or Value when Id is empty). Centralising it removes the repeated 4-way `||` chain
// from three ImGui draw functions (cutting their branch counts) and makes the match rule
// unit-testable without standing up ImGui.
//
// `filterLower` is expected to already be trimmed + lower-cased by the caller (the draw
// code computes it once per combo open via ToLowerAsciiCopy(TrimCopy(buf))). An empty
// filter matches every option, matching the original behaviour.
namespace TicketFieldEditorOptionFilterPure {

inline std::string EffectiveOptionId(const TrackerFieldOption& option) {
    return option.Id.empty() ? option.Value : option.Id;
}

inline bool OptionMatchesFilter(const TrackerFieldOption& option, const std::string& filterLower) {
    if (filterLower.empty()) {
        return true;
    }
    if (ToLowerAsciiCopy(option.Value).find(filterLower) != std::string::npos) {
        return true;
    }
    if (ToLowerAsciiCopy(option.SecondaryValue).find(filterLower) != std::string::npos) {
        return true;
    }
    return ToLowerAsciiCopy(EffectiveOptionId(option)).find(filterLower) != std::string::npos;
}

} // namespace TicketFieldEditorOptionFilterPure
