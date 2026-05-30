#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "ConfigManager.h"

/// Manages the set of ticket-grid views (filter / field-set / sort presets) for one tracker backend.
/// Implementation in Source/Core/src/Views.cpp (§4.8 item 45 — header-only anti-pattern resolved).
class Views {
  public:
    void EnsureLoaded(const TrackerConfig& cfg);

    const ViewsStore& GetStore() const { return Slice_; }
    ViewsStore& GetStoreMutable() { return Slice_; }

    const ViewDefinition* GetActiveView() const;
    ViewDefinition* GetActiveViewMutable();

    bool Activate(const std::string& viewId);
    bool Create(const ViewDefinition& prototype);
    bool UpdateActive(const ViewDefinition& updated);
    bool DeleteActive();
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
