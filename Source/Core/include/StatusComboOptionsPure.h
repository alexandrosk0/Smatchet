#pragma once

// StatusComboOptionsPure — which options the status combo offers (Quality Pillar 6). The live
// transition set, else the workflow remembered from earlier online use, else every status in the
// catalog; the current status is always present so the combo never opens empty. Pure, header-only.

#include "Tracker/TrackerFieldSchema.h"

#include <vector>

namespace smatchet {
namespace statuscombo {

enum class StatusOptionsSource : unsigned char { Live, Learned, Catalog };

struct StatusComboPick {
    std::vector<TrackerFieldOption> Options;
    StatusOptionsSource From = StatusOptionsSource::Catalog;
};

inline bool SameStatusOption(const TrackerFieldOption& a, const TrackerFieldOption& b) {
    return (!a.Id.empty() && a.Id == b.Id) || (!a.Value.empty() && a.Value == b.Value);
}

/// The live `targets` when `targetsAreLive` (even when empty: the tracker said there is no valid
/// move), else the learned `targets` when non-empty, else `catalogAll`; then `current` is prepended
/// unless an option with the same Id or Value is already listed. A current option with neither an Id
/// nor a Value is not added.
inline StatusComboPick PickStatusComboOptions(const std::vector<TrackerFieldOption>& targets, bool targetsAreLive,
                                              const std::vector<TrackerFieldOption>& catalogAll,
                                              const TrackerFieldOption& current) {
    StatusComboPick pick;
    if (targetsAreLive || !targets.empty()) {
        pick.Options = targets;
        pick.From = targetsAreLive ? StatusOptionsSource::Live : StatusOptionsSource::Learned;
    } else {
        pick.Options = catalogAll;
        pick.From = StatusOptionsSource::Catalog;
    }
    if (current.Id.empty() && current.Value.empty()) {
        return pick;
    }
    for (const TrackerFieldOption& opt : pick.Options) {
        if (SameStatusOption(current, opt)) {
            return pick;
        }
    }
    pick.Options.insert(pick.Options.begin(), current);
    return pick;
}

} // namespace statuscombo
} // namespace smatchet
