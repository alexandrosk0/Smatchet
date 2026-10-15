#pragma once

// CacheRecreateReportPure - the wording of a failed local-cache recreate, shared by the `cache.recreate`
// command and the Preferences "Delete and recreate" button. Header-only, C++14, no platform headers.
//
// The recreate can fail BEFORE it deleted anything (nothing changed) or AFTER (the cache files
// are gone but the fresh database could not be opened, or a -wal/-shm sidecar would not delete). Telling a user
// "was not recreated" in the second case is wrong: the cached rows and the queued offline edits are already lost.
//
// The recreate runs on a worker behind a "recreating" gate (AppController::StartRecreateLocalCacheDatabase), so its
// result arrives later as a CacheRecreateOutcome on the UI thread.

#include <string>

namespace smatchet {
namespace sync {

/// What a finished recreate did. `Wiped` is true as soon as any cache file was deleted, so it can be true even
/// when the recreate failed (a reopen failure, a sidecar that would not delete); `Error` is empty on success.
struct CacheRecreateOutcome {
    bool Ok = false;
    bool Wiped = false;
    std::string Error;
};

/// Why a recreate cannot start now ("" = it can): no database path is configured, the application is shutting
/// down, or another recreate is already running. A refusal changes nothing.
inline std::string CacheRecreateStartRefusal(bool dbPathEmpty, bool shuttingDown, bool alreadyRecreating) {
    if (dbPathEmpty) {
        return "Local cache database path is not set.";
    }
    if (shuttingDown) {
        return "Application is shutting down.";
    }
    if (alreadyRecreating) {
        return "The local cache is already being recreated.";
    }
    return std::string();
}

/// `wiped`: whether any cache file was deleted before the failure. `detail`: the error text of the failed step.
inline std::string DescribeCacheRecreateFailure(bool wiped, const std::string& detail) {
    if (!wiped) {
        return "Local cache was not recreated: " + detail;
    }
    return "Local cache was wiped (cached issues and queued offline edits are gone) but the new database could not "
           "be set up: " +
           detail + " Run sync.full or restart to refill it.";
}

/// The failure text for a finished recreate; "" when it succeeded.
inline std::string DescribeCacheRecreateOutcome(const CacheRecreateOutcome& outcome) {
    return outcome.Ok ? std::string() : DescribeCacheRecreateFailure(outcome.Wiped, outcome.Error);
}

} // namespace sync
} // namespace smatchet
