#pragma once

// Shared "Project" combobox used by the new-issue draft picker, the quick-create popup and the
// bulk-import modal. The popup body is the shared value dropdown (FilterDropdown.h): a filter field,
// a "Recent" section, then every project.
//   - Recent: the last projects picked for this tracker instance, from the shared recent-picks store
//     (RecentSelections.h, memory only). Listed even when the project list has not loaded.
//   - Projects: the tracker's list, loaded when the popup opens on the app-owned joined
//     background-task pool (smatchet::projects::LoadProjectList); later frames use the vector on the
//     picker state. The fetch captures a shared_ptr to the backend so a live tracker swap (which frees
//     the old backend) can't dangle it mid-fetch — see ADR 0012. Offline, or when the listing fails,
//     the list saved by the last successful listing is shown with a DataFreshnessCue and a Retry
//     (Quality Pillar 6); "No projects found." only ever describes a live, empty list.
//   - Projects whose field catalog is saved on this machine (FieldCatalogCache::ListCachedProjects(),
//     filtered to the current backend+endpoint) are added to the list when the tracker's list does not
//     hold them. They are read on the joined background-task pool once per popup open, so the file
//     read and JSON parse never run per frame (Quality Pillar 2).
// Pure UI helper: no global state, no allocations beyond what the search/render naturally needs.
// Renders inside the current ImGui scope — caller is responsible for ImGui::SetNextItemWidth
// upstream if a specific width is desired.

#include "FieldCatalogCache.h"
#include "TrackerFieldSchema.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ITrackerConnectivity;
class ITrackerBackend;
class AppController;
struct TrackerConfig;

namespace SmatchetProjectPicker {

/** Resolve the (backendKind, endpoint) identity pair the picker's saved-catalog filter
 *  keys on, from the active tracker config: backendKind matches
 *  FieldCatalogCache::CachedProjectEntry.backend; endpoint is the per-backend connection
 *  identity (Jira: domain; Plane: url|workspace; Linear: url|team). Shared by every
 *  Draw call site so the identity grammar can't drift between surfaces. */
void ResolveBackendKindAndEndpoint(const TrackerConfig& cfg, std::string& outBackendKind, std::string& outEndpoint);

/** Async fetch state for the project list. Picker state can be heap-owned by the
 *  caller (so the picker survives draw cycles), or stack-owned if the caller manages lifetime. */
struct State {
    // Fetched-from-server "all projects" list. Protected by `fetchMutex` because the fetch
    // thread writes into it; the UI thread reads under lock and copies once per frame.
    std::mutex fetchMutex;
    std::vector<RemoteProject> fetchedAll;
    std::atomic<bool> fetchInFlight{false};
    std::atomic<bool> fetchDone{false}; // true once a fetch returned (even if empty)
    std::string fetchError;
    bool fetchFailed = false;    // guarded by fetchMutex: the last listing failed or was skipped offline
    bool fetchFromSaved = false; // guarded by fetchMutex: fetchedAll is a saved copy (fallback or inside its lifetime)
    bool fetchSkippedOffline = false; // guarded by fetchMutex: the last load sent no request (offline)
    /// Set by Retry / Refresh (UI thread): the next load asks the tracker even while a saved list is still inside
    /// its lifetime. Consumed by StartAllProjectsFetch when it launches that load.
    std::atomic<bool> fetchBypassSaved{false};
    std::string fetchBackendKey;       // guarded by fetchMutex: the tracker the list was requested for
    std::uint64_t fetchGeneration = 0; // guarded by fetchMutex: bumped per load; a stale load publishes nothing
    // The saved-catalog projects, read off the UI thread once per popup open by StartCachedProjectsLoad.
    // cachedMutex guards the rows and cachedGeneration, which every read bumps so that a stale read
    // publishes nothing. popupWasOpen is the UI thread's open-edge latch.
    std::mutex cachedMutex;
    std::vector<FieldCatalogCache::CachedProjectEntry> cached;
    std::uint64_t cachedGeneration = 0;
    bool cachedLoaded = false; // guarded by cachedMutex: a read has landed (cached may still be empty)
    bool popupWasOpen = false;
    // UI thread: the recents scope whose Recent section was last offered its start from `cached`.
    std::string recentsSeedScope;
};

/** Start loading the project list for `state` on the app's background-task pool, unless a load is
 *  already running or has finished (Retry clears `fetchDone`). Offline it reads the saved list only; once
 *  the tracker is reachable again, a list that was skipped offline reloads. When the focused tracker is
 *  not the one the list was requested for, the rows are dropped, a load still running for the old
 *  tracker is superseded (its result is discarded), and the focused tracker's list loads. UI thread.
 *  Draw calls it on every frame the popup is open; tests call it directly. */
void StartAllProjectsFetch(State& state, AppController& app);

/** Re-read the saved-catalog projects for `state` on the app's background-task pool. Draw calls it when the
 *  popup opens; the rows already shown stay until the read lands. UI thread. */
void StartCachedProjectsLoad(State& state, AppController& app);

/** Draw the picker combobox.
 *
 *  @param idScope        Unique ImGui id scope (e.g. "draft_project" / "bulk_project"). Pushed
 *                        internally so multiple pickers may coexist on the same frame.
 *  @param state          Persistent picker state (owned by caller).
 *  @param app            App controller — launches the fetch on the joined task pool, and
 *                        supplies the active backend via `app.BackendShared()`. The project-list
 *                        fetch captures that shared_ptr so it survives a live tracker swap (ADR 0012).
 *                        Its focused cache-backend key scopes the recent picks.
 *  @param backendKind    "Jira" or "Plane" — matches FieldCatalogCache::CachedProjectEntry.backend.
 *  @param endpoint       Normalized endpoint string used to filter saved-catalog entries to the
 *                        current connection (Jira: domain; Plane: planeUrl + "|" + workspaceSlug).
 *  @param selectedKey    In/out: the currently selected project key. Empty == "(pick one)".
 *  @returns true iff the user picked / changed the selection this frame. */
bool Draw(const char* idScope, State& state, AppController& app, const std::string& backendKind,
          const std::string& endpoint, std::string& selectedKey);

} // namespace SmatchetProjectPicker
