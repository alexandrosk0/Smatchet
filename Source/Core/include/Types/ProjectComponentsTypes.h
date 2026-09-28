#pragma once

// ProjectComponentsTypes — what the per-project component cache answers (Quality Pillar 6): the
// project's options, live or restored from the lookup cache, plus the state the grid needs to draw a
// freshness cue. ProjectComponentsCacheService fills it; the components editor and cells read it.

#include "OfflineFirstPure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <memory>
#include <string>
#include <vector>

struct ProjectComponentsLookup {
    /// Live if Fresh, else saved from an earlier session; null when nothing is cached for the project.
    /// Shared and immutable, so a per-frame read copies a pointer, not the list.
    std::shared_ptr<const std::vector<TrackerFieldOption>> options;
    smatchet::offline::DataFreshness freshness = smatchet::offline::DataFreshness::UnavailableNoCache;
    bool inFlight = false;          ///< a fetch for the project is running
    bool lastAttemptFailed = false; ///< the most recent fetch failed
    std::string lastError;          ///< that failure's detail, for the cue tooltip
};
