#ifndef SMATCHET_INTERFACES_IAPP_SYNC_H
#define SMATCHET_INTERFACES_IAPP_SYNC_H

// Narrow facet for the backend-sync / local-cache-recovery surface — AppController fan-in
// (docs/plans/appcontroller-clusters-followup.md). AppController implements it; the sync.*
// command TU (Commands/Builtin/BuiltinCommands_Sync.cpp) depends on this instead of the full
// AppController.h. Rank-0 leaf (Interfaces/): TrackerConnectivityState comes from the rank-0
// Types/ConnectivityTypes.h and TrackerIssueFetchPack from the rank-0 Sync/SyncTypes.h (no
// back-edge); TrackerConfig / ViewsStore are pointer params, so a forward declaration suffices.
//
// Pillar-1 note: GetLastTrackerConnectivityState and GetLastTicketSyncWarning have per-frame
// callers (SmatchetGridHeaderUi / SmatchetStatusBarUi) but are already OUT-OF-LINE accessors on
// AppController, so virtualizing them costs at most one vtable lookup/frame — the same trade-off
// IAppTicketData::GetActiveTicketsSnapshot documents, not the inlined-hot-getter case the
// Pillar-1 exclusion protects. GetFieldCatalogError is header-inlined but its only callers are
// command TUs (never a draw loop), so its vtable slot is likewise off the hot path.

#include "SmatchetResult.h"          // VoidResult (RecreateLocalCacheDatabase)
#include "Sync/SyncTypes.h"          // TrackerIssueFetchPack (rank-0)
#include "Types/ConnectivityTypes.h" // TrackerConnectivityState (rank-0)

#include <string>

struct TrackerConfig; // pointer param — fwd-decl (concrete callers hold the full definition)
struct ViewsStore;    // pointer param — fwd-decl

class IAppSync {
  public:
    virtual ~IAppSync() = default;

    virtual void SyncWithBackend(const TrackerConfig* configOverride = nullptr,
                                 const ViewsStore* viewsOverride = nullptr) = 0;
    virtual VoidResult RecreateLocalCacheDatabase() = 0;
    virtual void RefreshLocalData() = 0;
    virtual TrackerIssueFetchPack FetchIssuesForActiveView(const TrackerConfig* configOverride = nullptr,
                                                           const ViewsStore* viewsOverride = nullptr) = 0;
    virtual TrackerConnectivityState GetLastTrackerConnectivityState() const = 0;
    virtual const std::string& GetLastTicketSyncWarning() const = 0;
    virtual const std::string& GetFieldCatalogError() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_SYNC_H
