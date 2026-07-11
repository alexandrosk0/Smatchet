#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "ConfigManager.h"

namespace smatchet {
namespace views_merge {

/// DR13a — fold out-of-band ToolbarAppend writes back into a to-be-written snapshot.
/// The SmatchetToolbarUi tracker-scope save worker rewrites ONLY
/// `ViewWorkspaceState::ToolbarAppend` for a backend, straight to disk, bypassing the
/// Views wrapper. Before Views::Save rewrites the whole file from its boot-time snapshot
/// it must re-read the file and merge those toolbar writes, or they are reverted (toolbar
/// buttons vanish on the next restart / backend swap). Ownership split: the Views wrapper
/// owns each backend's Views / ActiveViewId; the toolbar worker owns ToolbarAppend. A
/// backend present only on disk (created out of band) is preserved whole. Pure — no I/O,
/// bucket-A testable.
inline void MergePersistentViewsToolbarAppend(PersistentViewsFile& target, const PersistentViewsFile& fresh) {
    for (const auto& kv : fresh.Backends) {
        auto it = target.Backends.find(kv.first);
        if (it == target.Backends.end()) {
            target.Backends[kv.first] = kv.second;
        } else {
            it->second.ToolbarAppend = kv.second.ToolbarAppend;
        }
    }
}

/// DR13b — pick the new active-view index after erasing the active view at `erasedIndex`
/// from a list that now has `remainingCount` entries (caller guarantees >= 1). Keeps the
/// same slot when it still exists (deleting the last view lands on the new last), matching
/// the neighbour-select UX. Pure — bucket-A testable.
inline std::size_t PickActiveViewIndexAfterErase(std::size_t erasedIndex, std::size_t remainingCount) {
    return (erasedIndex < remainingCount) ? erasedIndex : (remainingCount - 1);
}

} // namespace views_merge
} // namespace smatchet

/// Manages the set of ticket-grid views (filter / field-set / sort presets) for one tracker backend.
/// Implementation in Source/Core/src/Views.cpp (§4.8 item 45 — header-only anti-pattern resolved).
class Views {
  public:
    void EnsureLoaded(const TrackerConfig& cfg);

    const ViewsStore& GetStore() const { return Slice_; }
    ViewsStore& GetStoreMutable() { return Slice_; }

    const std::unordered_map<std::string, ViewWorkspaceState>& GetDiskBackends() const { return Disk.Backends; }

    const ViewDefinition* GetActiveView() const;
    ViewDefinition* GetActiveViewMutable();

    bool Activate(const std::string& viewId);
    bool Create(const ViewDefinition& prototype);
    bool UpdateActive(const ViewDefinition& updated);
    bool DeleteActive();
    /// Delete the view with `id` regardless of whether it is the active view (DR13b —
    /// right-click Delete on a non-active row). Refuses to delete the last remaining view
    /// or an unknown id. When the active view is the one removed, a neighbour is selected
    /// as the new active view (same rule as DeleteActive, which now delegates here).
    bool Delete(const std::string& id);
    void Save();

    /// Monotonic counter bumped on every state mutation (Activate / Create /
    /// UpdateActive / DeleteActive / backend swap / explicit BumpRevision).
    /// Consumers cache view-derived data should include this in their cache
    /// key so in-place edits to the active view invalidate cleanly even when
    /// the active view id is unchanged.
    std::uint64_t GetRevision() const { return Revision_.load(); }

    /// External signal for paths that mutate via GetStoreMutable() /
    /// GetActiveViewMutable() — they must call this after their edit, since
    /// Views can't observe direct slice mutations.
    void BumpRevision() { Revision_.fetch_add(1); }

  private:
    void ApplyTrackerFromConfig(const TrackerConfig& cfg);
    static std::string BuildIdFromName(const std::string& name);
    bool Exists(const std::string& id) const;
    std::string BuildUniqueId(const std::string& base) const;

    bool Loaded = false;
    bool HasActiveBackend = false;
    std::string ActiveBackendKey;
    PersistentViewsFile Disk;
    ViewsStore Slice_;
    std::atomic<std::uint64_t> Revision_{0};
};
