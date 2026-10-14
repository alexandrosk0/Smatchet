#include "GridContextFieldCatalogOps.h"

#include "GridLiveContext.h"

#include <mutex>
#include <string>

namespace smatchet {
namespace catalogops {

// An explicit clear: the reset a tracker or site switch starts with. It supersedes every catalog fetch
// already in flight for this pane (CatalogEpoch), before the new backend is even installed, and empties
// only the in-memory catalog: the offline snapshot stays (Pillar 6), and no synthetic columns are
// published for a tracker whose catalog has not arrived yet. `clearUsers` also drops the user roster.
void ClearCatalogInMemoryInto(GridContextFieldCatalog& cat, bool clearUsers) {
    cat.CatalogEpoch.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        // SMATCHET_DEVIATION(rule=offline-cache-cleared; reason=site or backend switch only, not a failed fetch: the previous site's catalog must not show under this one, and the saved snapshot (never written here) restores this site's catalog when its fetch starts, offline included; owner=offline-sync; revisit=2027-09-30)
        cat.AvailableFields.clear();
        cat.AvailableComponents.clear();
        cat.AvailableIssueTypeMeta.clear();
        if (clearUsers) {
            // SMATCHET_DEVIATION(rule=offline-cache-cleared; reason=site or backend switch only: the previous site's user roster must not autocomplete under this one, and the saved roster (lookup_cache kind users, keyed by site) restores this site's users when its catalog fetch starts, offline included; owner=offline-sync; revisit=2027-09-30)
            cat.AvailableUsers.clear();
        }
        cat.currentCatalogProjectKey_.clear();
        cat.hasPendingCatalogProjectKey_ = false;
        cat.pendingCatalogProjectKey_.clear();
        cat.fieldCatalogEverLoaded_ = false;
        cat.fieldCatalogRestored_ = false;
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning.clear();
    }
    cat.TrackerFieldCatalogRevision.fetch_add(1);
}

bool WriteCatalogFailureBannerLocked(GridContextFieldCatalog& cat, catalogoffline::CatalogFailureBanner banner,
                                     const std::string& backendLabel, const std::string& error, bool errorTransient) {
    using catalogoffline::CatalogFailureBanner;
    bool changed = true;
    switch (banner) {
    case CatalogFailureBanner::WarningUsingCached: {
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        const std::string nextWarning =
            "Offline: using cached " + backendLabel + " field catalog. Last fetch failed: " + error;
        changed = nextWarning != cat.LastTrackerFieldCatalogWarning;
        cat.LastTrackerFieldCatalogWarning = nextWarning;
        break;
    }
    case CatalogFailureBanner::WarningRestoredSnapshot:
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning =
            "Offline: restored " + backendLabel + " field catalog from local snapshot. Last fetch failed: " + error;
        break;
    case CatalogFailureBanner::WarningSessionHadCatalog:
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning =
            "Offline: no field catalog snapshot could be loaded for this tracker context. Last fetch failed: " + error;
        break;
    case CatalogFailureBanner::ErrorKeepCatalog:
        // Non-retryable (auth / config / parse): the user must act, so show the error banner, but keep
        // the catalog. The grid holds pending edits instead of discarding them.
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogError = error;
        cat.LastTrackerFieldCatalogErrorTransient = false;
        break;
    case CatalogFailureBanner::ErrorNoCatalog:
        cat.fieldCatalogEverLoaded_ = false;
        cat.fieldCatalogRestored_ = false;
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogErrorTransient = errorTransient;
        cat.LastTrackerFieldCatalogError = errorTransient
                                               ? "No cached " + backendLabel + " field catalog available. " +
                                                     (error.empty() ? std::string("Last fetch failed.") : error)
                                               : error;
        break;
    }
    return changed;
}

} // namespace catalogops
} // namespace smatchet
