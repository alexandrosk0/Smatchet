#pragma once

// Shared "Project" combobox used by the new-issue draft picker and the bulk-import modal.
// Hybrid surface (OQ-2):
//   - Recently used: read directly from FieldCatalogCache::ListCachedProjects(), filtered to the
//     current backend+endpoint, ordered by lastUsedUnix desc.
//   - All projects: collapsible. First expand fires ITrackerConnectivity::ListProjects() on the
//     app-owned joined background-task pool; subsequent renders use the cached vector on the picker
//     state. The fetch captures a shared_ptr to the backend so a live tracker swap (which frees the
//     old backend) can't dangle it mid-fetch — see ADR 0012.
// Pure UI helper: no global state, no allocations beyond what the search/render naturally needs.
// Renders inside the current ImGui scope — caller is responsible for ImGui::SetNextItemWidth
// upstream if a specific width is desired.

#include "TrackerFieldSchema.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ITrackerConnectivity;
class ITrackerBackend;
class AppController;
struct TrackerConfig;

namespace SmatchetProjectPicker {

/** Resolve the (backendKind, endpoint) identity pair the picker's recently-used filter
 *  keys on, from the active tracker config: backendKind matches
 *  FieldCatalogCache::CachedProjectEntry.backend; endpoint is the per-backend connection
 *  identity (Jira: domain; Plane: url|workspace; Linear: url|team). Shared by every
 *  Draw call site so the identity grammar can't drift between surfaces. */
void ResolveBackendKindAndEndpoint(const TrackerConfig& cfg, std::string& outBackendKind, std::string& outEndpoint);

/** Async fetch state for the "All projects" lazy expand. Picker state can be heap-owned by the
 *  caller (so the picker survives draw cycles), or stack-owned if the caller manages lifetime. */
struct State {
    char searchBuf[128]{};
    bool allExpanded = false;
    // Fetched-from-server "all projects" list. Protected by `fetchMutex` because the fetch
    // thread writes into it; the UI thread reads under lock and copies once per frame.
    std::mutex fetchMutex;
    std::vector<RemoteProject> fetchedAll;
    std::atomic<bool> fetchInFlight{false};
    std::atomic<bool> fetchDone{false}; // true once a fetch returned (even if empty)
    std::string fetchError;
};

/** Draw the picker combobox.
 *
 *  @param idScope        Unique ImGui id scope (e.g. "draft_project" / "bulk_project"). Pushed
 *                        internally so multiple pickers may coexist on the same frame.
 *  @param state          Persistent picker state (owned by caller).
 *  @param app            App controller — launches the lazy fetch on the joined task pool, and
 *                        supplies the active backend via `app.BackendShared()`. The "All projects"
 *                        fetch captures that shared_ptr so it survives a live tracker swap (ADR 0012);
 *                        the backend also supplies backend+endpoint identity for the recently-used filter.
 *  @param backendKind    "Jira" or "Plane" — matches FieldCatalogCache::CachedProjectEntry.backend.
 *  @param endpoint       Normalized endpoint string used to filter recently-used entries to the
 *                        current connection (Jira: domain; Plane: planeUrl + "|" + workspaceSlug).
 *  @param selectedKey    In/out: the currently selected project key. Empty == "(pick one)".
 *  @returns true iff the user picked / changed the selection this frame. */
bool Draw(const char* idScope, State& state, AppController& app, const std::string& backendKind,
          const std::string& endpoint, std::string& selectedKey);

} // namespace SmatchetProjectPicker
