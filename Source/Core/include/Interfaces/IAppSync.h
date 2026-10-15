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

#include "OfflineFirstPure.h"        // IsOfflineState (rank-0)
#include "SmatchetResult.h"          // VoidResult (RecreateLocalCacheDatabase)
#include "Sync/SyncMode.h"           // SyncMode (rank-0)
#include "Sync/SyncSessionStats.h"   // SyncSessionStats (rank-0)
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
    /// SyncWithBackend with an explicit mode: `Delta` re-downloads only changed tickets, `Full` the whole view
    /// (`SyncWithBackend` itself means Delta). Non-pure; the default ignores the mode, so a facet fake that
    /// only counts syncs needs no stub.
    virtual void SyncWithBackendMode(const TrackerConfig* configOverride, const ViewsStore* viewsOverride,
                                     smatchet::sync::SyncMode mode) {
        (void)mode;
        SyncWithBackend(configOverride, viewsOverride);
    }
    /// Wipes the local cache database and opens a fresh one. `outWiped` says whether the wipe happened: true as soon
    /// as any cache file was deleted, so it can be true even when the call fails (a reopen failure, a sidecar
    /// that would not delete). On a failure with `outWiped` set the cached rows are gone: the call has already
    /// refreshed the focused pane and the queue snapshots, and the caller words the error as a partial wipe
    /// (see DescribeCacheRecreateFailure). On a failure with `outWiped` clear nothing was deleted.
    virtual VoidResult RecreateLocalCacheDatabase(bool& outWiped) = 0;
    /// Absolute path of the local cache database, for display; the raw configured path when it
    /// cannot be resolved.
    virtual std::string GetResolvedLocalCacheDbPath() const = 0;
    virtual void RefreshLocalData() = 0;
    /// RefreshLocalData whose SQLite read runs on a background task: call on the UI thread, which
    /// resolves the focused context and latches its backend generation; the swap-in is generation
    /// gated. Non-pure; the default falls back to the synchronous refresh (facet fakes).
    virtual void RefreshLocalDataAsync() { RefreshLocalData(); }
    virtual TrackerIssueFetchPack FetchIssuesForActiveView(const TrackerConfig* configOverride = nullptr,
                                                           const ViewsStore* viewsOverride = nullptr) = 0;
    virtual TrackerConnectivityState GetLastTrackerConnectivityState() const = 0;
    virtual const std::string& GetLastTicketSyncWarning() const = 0;
    virtual const std::string& GetFieldCatalogError() const = 0;
    /// What the focused pane's most recently finished sync session cost (mode, bodies fetched,
    /// requests, bytes, fallback reason). `Valid` is false until a session has finished. Non-pure
    /// so facet fakes need no stub; AppController answers from the focused pane's TicketSyncService.
    virtual smatchet::sync::SyncSessionStats GetLastSyncSessionStats() const { return {}; }

    /// Pillar 6: true while the last probe says the tracker is unreachable. Non-virtual on purpose.
    bool IsTrackerOffline() const { return smatchet::offline::IsOfflineState(GetLastTrackerConnectivityState()); }
};

#endif // SMATCHET_INTERFACES_IAPP_SYNC_H
