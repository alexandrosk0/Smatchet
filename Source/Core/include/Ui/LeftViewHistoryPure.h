#pragma once

// LeftViewHistoryPure — row filter and sorted field list for the left-view history window. Pure: no ImGui.

#include "LeftViewRecord.h"
#include "StringUtil.h"

#include <algorithm>
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

/// Label -> value for the details view, merged per field id with live values over saved ones. A field only one
/// side carries still shows; a field with no value on either side is not listed. Ids without a catalog label show
/// as they are, and fields that share a label are told apart by their id in brackets. Sorted by label.
inline std::vector<std::pair<std::string, std::string>>
MergeDetailFields(const std::unordered_map<std::string, std::string>& saved,
                  const std::unordered_map<std::string, std::string>& live,
                  const std::unordered_map<std::string, std::string>& labels) {
    std::map<std::string, std::string> byId;
    for (const auto& entry : saved) {
        if (!entry.second.empty()) {
            byId[entry.first] = entry.second;
        }
    }
    for (const auto& entry : live) {
        if (!entry.second.empty()) {
            byId[entry.first] = entry.second;
        }
    }
    const auto labelOf = [&labels](const std::string& id) {
        const auto it = labels.find(id);
        return it != labels.end() && !it->second.empty() ? it->second : id;
    };
    std::map<std::string, int> labelUses;
    for (const auto& entry : byId) {
        ++labelUses[labelOf(entry.first)];
    }
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(byId.size());
    for (const auto& entry : byId) {
        std::string label = labelOf(entry.first);
        if (labelUses[label] > 1 && label != entry.first) {
            label += " (" + entry.first + ")";
        }
        out.emplace_back(std::move(label), entry.second);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
                         return a.first < b.first;
                     });
    return out;
}

} // namespace smatchet
