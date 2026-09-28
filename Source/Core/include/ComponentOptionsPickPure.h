#pragma once

// ComponentOptionsPickPure — which option list the grid's components editor and cells use for a row
// (Quality Pillar 6). The row's own project list when one is cached, live or saved. Otherwise the
// field catalog's components, but only when the catalog was fetched for that same project: another
// project's catalog would offer components that do not exist in this one. Otherwise nothing, and the
// freshness cue explains why. Pure, header-only.

#include "OfflineFirstPure.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/ConnectivityTypes.h"
#include "Types/ProjectComponentsTypes.h"

#include <vector>

namespace smatchet {
namespace components {

struct ComponentOptionsPick {
    /// Never null. Points into `lookup`, `catalogOptions` or a static empty list, so it is valid for as
    /// long as the arguments of the PickComponentOptions call that returned it.
    const std::vector<TrackerFieldOption>* Options = nullptr;
    offline::DataFreshness Freshness = offline::DataFreshness::UnavailableNoCache;
    bool FromCatalog = false; ///< the list is the catalog's, standing in for the project's own
};

inline ComponentOptionsPick PickComponentOptions(const ProjectComponentsLookup& lookup, bool catalogIsRowProject,
                                                 const std::vector<TrackerFieldOption>& catalogOptions,
                                                 TrackerConnectivityState connectivity) {
    static const std::vector<TrackerFieldOption> kNoOptions;
    ComponentOptionsPick pick;
    if (lookup.options) {
        pick.Options = lookup.options.get();
        pick.Freshness = lookup.freshness;
        return pick;
    }
    if (catalogIsRowProject && !catalogOptions.empty()) {
        offline::FreshnessInputs in;
        in.HasCache = true;
        in.InFlight = lookup.inFlight;
        in.LastAttemptFailed = lookup.lastAttemptFailed;
        in.Connectivity = connectivity;
        pick.Options = &catalogOptions;
        pick.Freshness = offline::ClassifyFreshness(in);
        pick.FromCatalog = true;
        return pick;
    }
    pick.Options = &kNoOptions;
    pick.Freshness = lookup.freshness;
    return pick;
}

} // namespace components
} // namespace smatchet
