#pragma once

// LeftViewHistoryPure — row filter and sorted field list for the left-view history window. Pure: no ImGui.

#include "LeftViewRecord.h"
#include "StringUtil.h"

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smatchet {

/// True when the filter text appears in anything the row shows. An empty filter matches every row.
inline bool LeftViewRowMatches(const LeftViewRecord& row, const std::string& domainLabel, const std::string& filter) {
    return filter.empty() || ContainsCaseInsensitive(row.issueKey, filter) ||
           ContainsCaseInsensitive(row.summary, filter) || ContainsCaseInsensitive(row.viewName, filter) ||
           ContainsCaseInsensitive(domainLabel, filter) || ContainsCaseInsensitive(row.reason, filter) ||
           ContainsCaseInsensitive(row.author, filter);
}

/// Label -> value for the details view, live values over saved ones. A field only one side carries still shows.
/// Ids without a catalog label show as they are. Sorted by label.
inline std::vector<std::pair<std::string, std::string>>
MergeDetailFields(const std::unordered_map<std::string, std::string>& saved,
                  const std::unordered_map<std::string, std::string>& live,
                  const std::unordered_map<std::string, std::string>& labels) {
    std::map<std::string, std::string> merged;
    const auto labelOf = [&labels](const std::string& id) {
        const auto it = labels.find(id);
        return it != labels.end() && !it->second.empty() ? it->second : id;
    };
    for (const auto& entry : saved) {
        merged[labelOf(entry.first)] = entry.second;
    }
    for (const auto& entry : live) {
        if (!entry.second.empty()) {
            merged[labelOf(entry.first)] = entry.second;
        }
    }
    return std::vector<std::pair<std::string, std::string>>(merged.begin(), merged.end());
}

} // namespace smatchet
