#pragma once

// SyncMode - how a streaming sync should fetch (docs/plans/active/delta-sync.md § Phase 3).
// Rank-0 leaf shared by the app-sync facet (Interfaces/IAppSync.h), the sync service and the UI.

namespace smatchet {
namespace sync {

enum class SyncMode {
    /// Fetch the roster (keys + versions), download only the tickets that changed, serve the rest from
    /// the cache. Any failure on that path falls back to Full. The default everywhere.
    Delta,
    /// Re-download the whole view, exactly as before delta sync existed. Shift+Refresh and `sync.full`.
    Full,
};

/// Merges two requests that are queued behind a running session: a forced Full always wins, so a
/// "re-download everything" click is never downgraded by a later ordinary refresh.
inline SyncMode CombineSyncModes(SyncMode queued, SyncMode incoming) {
    return (queued == SyncMode::Full || incoming == SyncMode::Full) ? SyncMode::Full : SyncMode::Delta;
}

} // namespace sync
} // namespace smatchet
