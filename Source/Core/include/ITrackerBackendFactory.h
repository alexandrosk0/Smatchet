#pragma once

#include <memory>
#include <string>

class ITrackerBackend;
struct TrackerConfig;

/// Abstraction that creates concrete `ITrackerBackend` instances by tracker-type name.
/// `AppController` owns one of these and asks it for a backend whenever the active tracker
/// type changes. `main.cpp` (or an embedding host such as Unreal) may inject a custom
/// factory via `SetBackendFactory` before `Initialize`; otherwise the controller falls back
/// to `DefaultTrackerBackendFactory` on first use.
/// The seam exists so tests and embedding hosts can substitute a mock backend without
/// touching `AppController.cpp`, and so adding a third backend doesn't require editing
/// both the `Initialize` and `SyncWithBackend` instantiation sites.
class ITrackerBackendFactory {
  public:
    virtual ~ITrackerBackendFactory() = default;

    /// Create a backend for the given tracker-type string, built from the CALLER's live
    /// `cfg` — implementations must NOT re-read config from disk (issue #979: the prefs
    /// "Save & Sync" path calls this while the debounced config write is still in flight,
    /// so a disk re-read races the save and constructs clients with stale/empty
    /// credentials).
    /// Lookup is case-insensitive ("Jira", "jira", "JIRA" all match). Implementations
    /// should fall back to a sensible default (`JiraClient` in the default impl) rather
    /// than returning `nullptr` for unknown input — `AppController` treats a null return
    /// as a hard error and aborts initialization.
    virtual std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType, const TrackerConfig& cfg) = 0;
};
