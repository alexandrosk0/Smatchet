#pragma once

// Pure freshness rules for saved lookups (docs/plans/active/delta-sync.md § Phase 7): the saved project list,
// the saved field catalog and other rows keep a save time, and a row younger than its lifetime is served
// without a live fetch. Header-only, no I/O.

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace smatchet {
namespace sync {

/// True when a row saved at `savedAtEpochSec` is still within `ttlSec` at `nowEpochSec`. An unknown save time
/// (<= 0), a non-positive lifetime or a save time in the future (clock moved back) is never fresh: the caller
/// fetches live.
inline bool IsLookupRowFresh(std::int64_t savedAtEpochSec, std::int64_t nowEpochSec, std::int64_t ttlSec) {
    if (savedAtEpochSec <= 0 || ttlSec <= 0 || nowEpochSec < savedAtEpochSec) {
        return false;
    }
    return nowEpochSec - savedAtEpochSec < ttlSec;
}

/// Seconds until a row saved at `savedAtEpochSec` expires (0 when it already has or its age is unknown).
inline std::int64_t SecondsUntilLookupStale(std::int64_t savedAtEpochSec, std::int64_t nowEpochSec,
                                            std::int64_t ttlSec) {
    if (!IsLookupRowFresh(savedAtEpochSec, nowEpochSec, ttlSec)) {
        return 0;
    }
    return ttlSec - (nowEpochSec - savedAtEpochSec);
}

/// True when the grid may skip the startup field-catalog fetch: the saved snapshot is within its lifetime AND
/// every column of the active view resolves against it (a column the snapshot lacks needs a live catalog), AND
/// the snapshot was saved for the project the active view resolves to. A fresh UNSCOPED snapshot (the startup
/// fallback when no project-scoped one existed) carries no project-scoped data such as component options, so it
/// never stands in for the project-scoped fetch: `snapshotProjectKey` and `viewProjectKey` must be equal.
/// Blank ids and the synthetic "id" column are ignored.
inline bool CanSkipCatalogFetch(std::int64_t savedAtEpochSec, std::int64_t nowEpochSec, std::int64_t ttlSec,
                                const std::vector<std::string>& viewFieldIds,
                                const std::vector<std::string>& catalogFieldIds, const std::string& snapshotProjectKey,
                                const std::string& viewProjectKey) {
    if (catalogFieldIds.empty() || snapshotProjectKey != viewProjectKey ||
        !IsLookupRowFresh(savedAtEpochSec, nowEpochSec, ttlSec)) {
        return false;
    }
    const std::unordered_set<std::string> known(catalogFieldIds.begin(), catalogFieldIds.end());
    for (const std::string& id : viewFieldIds) {
        if (id.empty() || id == "id") {
            continue;
        }
        if (known.find(id) == known.end()) {
            return false;
        }
    }
    return true;
}

} // namespace sync
} // namespace smatchet
