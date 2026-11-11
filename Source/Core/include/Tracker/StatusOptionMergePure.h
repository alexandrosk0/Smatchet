#pragma once

// StatusOptionMergePure — one list row per status name. Jira workflows can each define their own
// "In Progress" with a different id, so /rest/api/3/status and an issue's /transitions can both name a
// status twice. Jira only: its writes pick a transition from the issue's own list, while Linear and
// Plane send the option's id as it is, so merging there would write another team's or project's state.
// Pure, header-only.

#include "StringUtil.h"
#include "Tracker/TrackerFieldSchema.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smatchet {
namespace statusoptions {

inline bool SameStatusName(const std::string& a, const std::string& b) {
    return !a.empty() && EqualsIgnoreCaseAscii(a, b);
}

/// Keeps the first option of each name (compared case-insensitively, first spelling and order kept) and
/// records the ids of the rest in its MergedIds, so an id from an issue's own transitions still resolves.
/// Options without a name are kept as they are. Safe to run again on a merged list.
inline void MergeSameNameStatusOptions(std::vector<TrackerFieldOption>& options) {
    std::unordered_map<std::string, std::size_t> firstByName;
    std::vector<TrackerFieldOption> kept;
    kept.reserve(options.size());
    for (TrackerFieldOption& option : options) {
        if (option.Value.empty()) {
            kept.push_back(std::move(option));
            continue;
        }
        const auto inserted = firstByName.emplace(ToLowerAsciiCopy(option.Value), kept.size());
        if (inserted.second) {
            kept.push_back(std::move(option));
            continue;
        }
        TrackerFieldOption& first = kept[inserted.first->second];
        std::vector<std::string> ids = std::move(option.MergedIds);
        ids.push_back(std::move(option.Id));
        for (std::string& id : ids) {
            if (!id.empty() && !first.HasId(id)) {
                first.MergedIds.push_back(std::move(id));
            }
        }
    }
    options = std::move(kept);
}

/// The same for a catalog status field, keeping its flat AllowedValues in step with the options.
inline void MergeSameNameStatusOptions(TrackerField& statusField) {
    MergeSameNameStatusOptions(statusField.AllowedValueOptions);
    statusField.AllowedValues.clear();
    for (const TrackerFieldOption& option : statusField.AllowedValueOptions) {
        if (!option.Value.empty()) {
            statusField.AllowedValues.push_back(option.Value);
        }
    }
}

} // namespace statusoptions
} // namespace smatchet
