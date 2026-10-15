#pragma once

// ProjectListLookup — the project picker's "All projects" read with an offline copy (Quality Pillar 6).
// A live listing is saved to the lookup cache (kind `projects`); when the tracker is offline, or the
// listing fails, the saved list is shown instead of an empty one, together with the failure so the UI
// can say why and offer a retry. Worker-only: it may wait on the tracker and it reads and writes the
// lookup cache.

#include "Tracker/TrackerError.h"
#include "Tracker/TrackerFieldSchema.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ILookupCache;
class ITrackerConnectivity;

namespace smatchet {
namespace projects {

struct ProjectListOutcome {
    std::vector<RemoteProject> Projects;
    bool FromSaved = false;   ///< Projects is the list an earlier successful listing saved
    bool Failed = false;      ///< the live listing failed, or was skipped because the tracker is offline
    TrackerError Error;       ///< why it failed (Transport when it was skipped offline)
    bool ServedFresh = false; ///< the saved list was younger than the lifetime: no live request was sent
};

/// True when `Projects` is a copy saved earlier rather than a listing the tracker just answered: the picker draws
/// it under the "saved" cue (with a Refresh), whether it was shown because the listing failed (FromSaved) or
/// because it was still inside its lifetime (ServedFresh).
inline bool IsSavedProjectCopy(const ProjectListOutcome& outcome) { return outcome.FromSaved || outcome.ServedFresh; }

/// Offline: the saved list, with no request. Online: the live list, saved on success; on a failure the
/// saved list (when there is one) plus the error. A null `store` or an empty `backendKey` saves and
/// restores nothing. Never throws.
/// Online with `ttlSec` > 0: a saved list younger than `ttlSec` (at `nowEpochSec`, 0 = the real clock) is
/// returned as is, without a request (`ServedFresh`; not `FromSaved`, not `Failed`; still a saved copy, see
/// IsSavedProjectCopy). An empty saved list is never served that way. `ttlSec` 0 always asks the tracker.
ProjectListOutcome LoadProjectList(ITrackerConnectivity& connectivity, const std::shared_ptr<ILookupCache>& store,
                                   const std::string& backendKey, bool offline, std::int64_t ttlSec = 0,
                                   std::int64_t nowEpochSec = 0);

} // namespace projects
} // namespace smatchet
