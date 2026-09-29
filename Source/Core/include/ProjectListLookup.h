#pragma once

// ProjectListLookup — the project picker's "All projects" read with an offline copy (Quality Pillar 6).
// A live listing is saved to the lookup cache (kind `projects`); when the tracker is offline, or the
// listing fails, the saved list is shown instead of an empty one, together with the failure so the UI
// can say why and offer a retry. Worker-only: it may wait on the tracker and it reads and writes the
// lookup cache.

#include "Tracker/TrackerError.h"
#include "Tracker/TrackerFieldSchema.h"

#include <memory>
#include <string>
#include <vector>

class ILookupCache;
class ITrackerConnectivity;

namespace smatchet {
namespace projects {

struct ProjectListOutcome {
    std::vector<RemoteProject> Projects;
    bool FromSaved = false; ///< Projects is the list an earlier successful listing saved
    bool Failed = false;    ///< the live listing failed, or was skipped because the tracker is offline
    TrackerError Error;     ///< why it failed (Transport when it was skipped offline)
};

/// Offline: the saved list, with no request. Online: the live list, saved on success; on a failure the
/// saved list (when there is one) plus the error. A null `store` or an empty `backendKey` saves and
/// restores nothing. Never throws.
ProjectListOutcome LoadProjectList(ITrackerConnectivity& connectivity, const std::shared_ptr<ILookupCache>& store,
                                   const std::string& backendKey, bool offline);

} // namespace projects
} // namespace smatchet
