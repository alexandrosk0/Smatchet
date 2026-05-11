#pragma once

#include <string>
#include <vector>
#include "ConfigManager.h"

/// Manages the set of ticket-grid views (filter / field-set / sort presets) for one tracker backend.
/// Implementation in Source_Core/src/Views.cpp (§4.8 item 45 — header-only anti-pattern resolved).
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
};
