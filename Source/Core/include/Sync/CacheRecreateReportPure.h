#pragma once

// CacheRecreateReportPure - the wording of a failed local-cache recreate, shared by the `cache.recreate`
// command and the Preferences "Delete and recreate" button. Header-only, C++14, no platform headers.
//
// RecreateLocalCacheDatabase can fail BEFORE it deleted anything (nothing changed) or AFTER (the cache files
// are gone but the fresh database could not be opened, or a -wal/-shm sidecar would not delete). Telling a user
// "was not recreated" in the second case is wrong: the cached rows and the queued offline edits are already lost.

#include <string>

namespace smatchet {
namespace sync {

/// `wiped`: whether any cache file was deleted before the failure. `detail`: the error text of the failed step.
inline std::string DescribeCacheRecreateFailure(bool wiped, const std::string& detail) {
    if (!wiped) {
        return "Local cache was not recreated: " + detail;
    }
    return "Local cache was wiped (cached issues and queued offline edits are gone) but the new database could not "
           "be set up: " +
           detail + " Run sync.full or restart to refill it.";
}

} // namespace sync
} // namespace smatchet
