#ifndef SMATCHET_TYPES_CATALOG_FETCH_TYPES_H
#define SMATCHET_TYPES_CATALOG_FETCH_TYPES_H

// Types/CatalogFetchTypes.h — what the grid's field-catalog fetch latches when it starts (#2328). A leaf the
// UI session header can include without AppController.h (fan-in).

#include <cstdint>
#include <string>

struct GridLiveContext;

/// Taken on the UI thread when the grid starts a field-catalog fetch (AppController::LatchGridCatalogFetch) and
/// handed back with its result (AppController::ApplyGridFieldCatalogResult). The result lands in this pane — not in
/// whichever pane is focused when it arrives — and only while the pane still runs the same backend and site, its
/// catalog has not been cleared since, and no RefreshFieldCatalog (a draft's project catalog) started after it;
/// otherwise it is dropped.
struct GridCatalogFetchLatch {
    /// The pane the fetch is for. A retired pane stays alive as a husk until ~AppController, so the pointer
    /// outlives any fetch.
    GridLiveContext* Pane = nullptr;
    std::uint64_t PaneGeneration = 0; ///< the pane's backend generation at launch
    std::uint64_t CatalogEpoch = 0;   ///< the pane's catalog epoch at launch
    std::uint64_t RefreshSeq = 0;     ///< the pane's RefreshFieldCatalog sequence at launch (not bumped)
    std::string PaneCacheKey;         ///< the pane's cache namespace (tracker site) at launch
};

#endif // SMATCHET_TYPES_CATALOG_FETCH_TYPES_H
