#pragma once

#include "Tracker/TrackerFieldSchema.h"

#include <vector>

namespace smatchet::statuscombo {

enum class StatusOptionsSource : unsigned char { Live, Learned, Catalog };

struct StatusComboPick {
    std::vector<TrackerFieldOption> Options;
    StatusOptionsSource From = StatusOptionsSource::Catalog;
};

inline StatusComboPick PickStatusComboOptions(const std::vector<TrackerFieldOption>& targets,
                                              bool targetsAreLive,
                                              const std::vector<TrackerFieldOption>& catalogAll,
                                              const TrackerFieldOption& current) {
    StatusComboPick pick;

    // Use targets if non-empty; otherwise fall back to catalog.
    if (!targets.empty()) {
        pick.Options = targets;
        pick.From = targetsAreLive ? StatusOptionsSource::Live : StatusOptionsSource::Learned;
    } else {
        pick.Options = catalogAll;
        pick.From = StatusOptionsSource::Catalog;
    }

    // Prepend current unless an option with the same id or value is already present.
    if (!current.Id.empty() || !current.Value.empty()) {
        bool found = false;
        for (const auto& opt : pick.Options) {
            if (((!current.Id.empty() && !opt.Id.empty()) && current.Id == opt.Id) ||
                ((!current.Value.empty() && !opt.Value.empty()) && current.Value == opt.Value)) {
                found = true;
                break;
            }
        }
        if (!found) {
            pick.Options.insert(pick.Options.begin(), current);
        }
    }

    return pick;
}

} // namespace smatchet::statuscombo
