#pragma once

// ProjectComponentsCacheService — the per-project Jira component options behind the grid's components
// editor and component cell names (Quality Pillar 6). One KeyedLookupCache keyed by (backend namespace,
// project key) replaces the per-pane in-flight and retry-after maps that the lazy loader and the
// post-sync warm each hand-rolled. It never fetches while the tracker is offline, backs off after a
// failure, retries on reconnect, and clears the in-flight flag on every exit, a throw included.
// Every successful fetch is saved to the lookup cache on the worker (kind `project_components`) and the
// saved rows are loaded once per backend, so the editor still offers a project's components while the
// tracker is unreachable.
// Entries belong to the backend, not the pane: panes on one tracker share them, and a focus switch in
// the middle of a fetch cannot strand a pane on "Loading" (the #975 class).
// Lifetime mirrors IssueTransitionsCacheService: AppController owns the service via std::unique_ptr and
// outlives it; workers launched via deps_.LaunchBackgroundTask are joined in ~AppController before the
// deps adapter dies.

#include "Config/ConfigManager.h"
#include "KeyedLookupCache.h"
#include "StoreLoadLatch.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/ProjectComponentsTypes.h"

#include <memory>
#include <string>
#include <vector>

class IEditMetaDeps;
class ILookupCache;
class ITrackerBackend;

class ProjectComponentsCacheService {
  public:
    explicit ProjectComponentsCacheService(IEditMetaDeps& deps);

    /// The focused backend's cached options for `projectKey`, without touching the network. Cheap;
    /// safe every frame.
    ProjectComponentsLookup GetComponentOptions(const std::string& projectKey) const;

    /// Start a background fetch for `projectKey` when one is allowed (not live, not in flight, not
    /// offline, not inside the failure backoff) and load the saved rows once per backend. Non-blocking.
    /// True when this call changed the entry: a fetch started, or failed to start and was recorded as
    /// a failure. The caller then re-reads it (GetComponentOptions) to show the new state this frame.
    bool EnsureComponentsLoaded(const std::string& projectKey);

    /// Fetch, one after another on a single worker, every project seen in the active tickets (the
    /// post-sync warm). A project that is live, in flight or backing off when its turn comes is skipped.
    void WarmForActiveTicketsAsync(TrackerConfig trackerCfgForWorker);

    /// Connectivity came back: clear every failure backoff so the next use retries.
    void OnConnectivityRecovered();

  private:
    using OptionsPtr = std::shared_ptr<const std::vector<TrackerFieldOption>>;
    using OptionsCache = smatchet::offline::KeyedLookupCache<OptionsPtr>;

    void EnsureSavedLoaded(const std::string& backendKey);
    /// Worker body: fetch one project for a ticket taken with TryBeginFetch, record the outcome, and
    /// save a success. `cfgSnapshot` null → ConfigManager::Load() on the worker.
    void FetchOne(const std::shared_ptr<ITrackerBackend>& backend, const OptionsCache::Ticket& ticket,
                  const std::string& backendKey, const std::string& projectKey, const TrackerConfig* cfgSnapshot,
                  const std::shared_ptr<ILookupCache>& store);

    IEditMetaDeps& deps_;
    OptionsCache cache_;
    /// Backends whose saved rows were loaded (or are loading).
    smatchet::offline::StoreLoadLatch savedLoad_;
};
