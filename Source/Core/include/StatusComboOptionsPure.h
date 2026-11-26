#pragma once

// StatusComboOptionsPure — which options the status combo offers (Quality Pillar 6). The live
// transition set, else the workflow remembered from earlier online use, else every status in the
// catalog; the current status is always present so the combo never opens empty. Pure, header-only.

#include "OfflineFirstPure.h"
#include "Tracker/StatusOptionMergePure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <cstddef>
#include <vector>

namespace smatchet {
namespace statuscombo {

enum class StatusOptionsSource : unsigned char { Live, Learned, Catalog };

struct StatusComboPick {
    std::vector<TrackerFieldOption> Options;
    StatusOptionsSource From = StatusOptionsSource::Catalog;
};

inline bool SameStatusOption(const TrackerFieldOption& a, const TrackerFieldOption& b) {
    return (!a.Id.empty() && a.Id == b.Id) || statusoptions::SameStatusName(a.Value, b.Value);
}

/// The live `targets` when `targetsAreLive` (even when empty: the tracker said there is no valid
/// move), else the learned `targets` when non-empty, else `catalogAll`, with each status name listed
/// once (the first option keeps its id, which came from that same list); then `current` is prepended
/// unless an option with the same Id or name is already listed. A listed option of the same name takes
/// the current Id: that row is the current status, so the combo highlights it and ignores a re-pick
/// instead of writing a move to a look-alike. A current option with neither an Id nor a Value is not
/// added.
inline StatusComboPick PickStatusComboOptions(const std::vector<TrackerFieldOption>& targets, bool targetsAreLive,
                                              const std::vector<TrackerFieldOption>& catalogAll,
                                              const TrackerFieldOption& current) {
    StatusComboPick pick;
    if (targetsAreLive || !targets.empty()) {
        pick.Options = targets;
        pick.From = targetsAreLive ? StatusOptionsSource::Live : StatusOptionsSource::Learned;
        statusoptions::MergeSameNameStatusOptions(pick.Options);
    } else {
        // The catalog's status list is merged once where it is built and where it is loaded from
        // cache, so it is not re-merged here every frame the combo is open.
        pick.Options = catalogAll;
        pick.From = StatusOptionsSource::Catalog;
    }
    if (current.Id.empty() && current.Value.empty()) {
        return pick;
    }
    for (TrackerFieldOption& opt : pick.Options) {
        if (SameStatusOption(current, opt)) {
            if (!current.Id.empty()) {
                opt.Id = current.Id;
            }
            return pick;
        }
    }
    pick.Options.insert(pick.Options.begin(), current);
    return pick;
}

/// The less live of two sources (Live < Learned < Catalog): what a list built from both rows can claim.
inline StatusOptionsSource LessLiveSource(StatusOptionsSource a, StatusOptionsSource b) {
    return static_cast<unsigned char>(a) >= static_cast<unsigned char>(b) ? a : b;
}

/// The staler of two freshness values (the declaration order: Fresh is the freshest, UnavailableNoCache the
/// stalest): what a list built from both rows can claim.
inline smatchet::offline::DataFreshness LeastFresh(smatchet::offline::DataFreshness a,
                                                   smatchet::offline::DataFreshness b) {
    return static_cast<unsigned char>(a) >= static_cast<unsigned char>(b) ? a : b;
}

/// The statuses every list offers, in the first list's order (each one with the first list's id). Two options
/// are the same status by SameStatusOption: the same id, or the same name. Empty when `lists` is empty or when no
/// status is in all of them. Used by the bulk-edit picker: each selected row offers its own lists (its current
/// status stays in it, see PickStatusComboOptions), so a row already at a status never rules that status out.
inline std::vector<TrackerFieldOption>
IntersectStatusOptions(const std::vector<std::vector<TrackerFieldOption>>& lists) {
    std::vector<TrackerFieldOption> common;
    if (lists.empty()) {
        return common;
    }
    for (const TrackerFieldOption& candidate : lists.front()) {
        bool inAll = true;
        for (std::size_t i = 1; i < lists.size() && inAll; ++i) {
            bool found = false;
            for (const TrackerFieldOption& opt : lists[i]) {
                if (SameStatusOption(candidate, opt)) {
                    found = true;
                    break;
                }
            }
            inAll = found;
        }
        if (inAll) {
            common.push_back(candidate);
        }
    }
    return common;
}

} // namespace statuscombo
} // namespace smatchet
