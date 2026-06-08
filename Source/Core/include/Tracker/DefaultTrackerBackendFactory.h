#pragma once

#include "ITrackerBackendFactory.h"

/// In-tree default factory. Knows about the two shipped backends (`JiraClient`,
/// `PlaneClient`) and falls back to Jira on unknown input.
/// Implementation lives in `DefaultTrackerBackendFactory.cpp` so this header does NOT
/// transitively drag `JiraClient.h` / `PlaneClient.h` into every TU that wants to know
/// the factory exists.
class DefaultTrackerBackendFactory : public ITrackerBackendFactory {
  public:
    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType, const TrackerConfig& cfg) override;
};
