#include "SmatchetProjectPicker.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "DataFreshnessCue.h"
#include "FieldCatalogCache.h"
#include "FilterDropdown.h"
#include "ITrackerBackend.h"
#include "Logger.h"
#include "OfflineFirstPure.h"
#include "ProjectListLookup.h"
#include "RecentSelections.h"
#include "ScopeExit.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include "SmatchetProjectPicker_detail.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace SmatchetProjectPicker {

void ResolveBackendKindAndEndpoint(const TrackerConfig& cfg, std::string& outBackendKind, std::string& outEndpoint) {
    // The pair the snapshot cache indexes each catalog under, so the list shows this tracker's projects.
    FieldCatalogCache::FieldCatalogIndexIdentity id = FieldCatalogCache::BuildFieldCatalogIndexIdentity(cfg);
    outBackendKind = std::move(id.backend);
    outEndpoint = std::move(id.endpoint);
}

namespace {

// What the popup lists of the tracker's project list this frame.
struct ProjectListView {
    std::vector<RemoteProject> Projects; // live or saved rows; empty while there is nothing to show
    smatchet::offline::DataFreshness Freshness = smatchet::offline::DataFreshness::Fresh;
};

// Above the list: the freshness cue for the tracker's project list, the error when there is nothing to
// show, and Retry / Refresh. Returns the rows to list. Runs inside the active BeginCombo scope.
ProjectListView DrawProjectListStatus(State& state, AppController& app) {
    ProjectListView view;
    std::string fetchError;
    bool failed = false;
    bool fromSaved = false;
    {
        std::lock_guard<std::mutex> lk(state.fetchMutex);
        view.Projects = state.fetchedAll;
        fetchError = state.fetchError;
        failed = state.fetchFailed;
        fromSaved = state.fetchFromSaved;
    }
    const bool inFlight = state.fetchInFlight.load();
    const bool done = state.fetchDone.load();
    smatchet::offline::FreshnessInputs in;
    in.HasCache = !view.Projects.empty() || fromSaved || (done && !failed);
    in.Live = done && !failed && !fromSaved;
    in.InFlight = inFlight;
    in.LastAttemptFailed = failed;
    in.Connectivity = app.GetLastTrackerConnectivityState();
    view.Freshness = smatchet::offline::ClassifyFreshness(in);
    const bool showRows = smatchet::offline::ShouldRenderContent(view.Freshness);
    const float cursorBefore = ImGui::GetCursorPosY();
    // Pillar 6: saved or stale rows stay visible under the cue; the failure is its tooltip.
    DataFreshnessCue::Draw(view.Freshness, fetchError.empty() ? nullptr : fetchError.c_str());
    if (failed && !inFlight) {
        // P2-M11: with nothing to show, the error itself is the content — a bad or expired token
        // must never read as an empty project list. Retry re-kicks the load next frame.
        if (!showRows && !fetchError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, SmatchetTheme::GetActiveSemanticColors().ErrorText);
            ImGui::TextWrapped("%s", SmatchetLocalization::Format("draft.project.fetch_failed",
                                                                  "Couldn't load projects: %s", fetchError.c_str()));
            ImGui::PopStyleColor();
        }
        if (ImGui::SmallButton(SmatchetLocalization::T("draft.project.retry", "Retry"))) {
            state.fetchBypassSaved.store(true);
            state.fetchDone.store(false);
        }
    } else if (fromSaved && !inFlight) {
        // A saved copy served inside its lifetime looks complete but may be out of date: Refresh asks the
        // tracker now.
        if (ImGui::SmallButton(SmatchetLocalization::T("common.refresh", "Refresh"))) {
            state.fetchBypassSaved.store(true);
            state.fetchDone.store(false);
        }
    }
    if (ImGui::GetCursorPosY() != cursorBefore) {
        ImGui::Separator();
    }
    if (!showRows) {
        view.Projects.clear();
    }
    return view;
}

// One row of the popup: the key a pick stores and the text shown.
struct ProjectRow {
    std::string Key;
    std::string Label;
};

// The rows of the popup: the tracker's projects, then the projects whose field catalog is saved on this
// machine (the ones opened before) that the tracker's list does not hold, so those stay pickable
// before the list loads and while offline.
std::vector<ProjectRow> BuildProjectRows(State& state, const std::vector<RemoteProject>& projects,
                                         const std::string& backendKind, const std::string& endpoint) {
    std::vector<ProjectRow> rows;
    rows.reserve(projects.size());
    for (const auto& p : projects) {
        ProjectRow row;
        row.Key = detail::AllProjectKey(p, backendKind);
        if (!detail::AllProjectPasses(row.Key, p, std::string())) {
            continue;
        }
        row.Label = detail::MakeRowLabel(p, backendKind);
        rows.push_back(std::move(row));
    }
    const std::size_t listed = rows.size();
    std::lock_guard<std::mutex> lk(state.cachedMutex);
    for (const auto& e : state.cached) { // at most the catalog cache's cap (16) small rows
        if (!detail::RecentEntryPasses(e, backendKind, endpoint, std::string())) {
            continue;
        }
        const bool known = std::any_of(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(listed),
                                       [&e](const ProjectRow& row) { return row.Key == e.projectKey; });
        if (!known) {
            rows.push_back(ProjectRow{e.projectKey, e.projectKey});
        }
    }
    return rows;
}

// Before any pick is recorded for a tracker (first run of a build with recent picks, or a new
// connection), its Recent section starts from the projects whose catalog is saved on this machine,
// the ones opened before, most recently used first. Looked at once per scope, when the tracker's
// project list and the saved-catalog read are both here; the dropdown reads its recents when it opens,
// so the section shows from the next open. Only projects in `projects`, the focused tracker's own list,
// are taken: the scope is that tracker's, while the saved catalogs are matched on the form's connection,
// and in a split over two trackers those can differ.
void SeedRecentsFromSavedCatalogs(State& state, const std::string& backendKey,
                                  const std::vector<RemoteProject>& projects, const std::string& backendKind,
                                  const std::string& endpoint) {
    if (backendKey.empty() || projects.empty()) {
        return; // no tracker yet, or its list is not here yet
    }
    const std::string scope = smatchet::recents::MakeScopeKey(backendKey, "project");
    if (state.recentsSeedScope == scope) {
        return;
    }
    std::vector<FieldCatalogCache::CachedProjectEntry> saved;
    {
        std::lock_guard<std::mutex> lk(state.cachedMutex);
        if (!state.cachedLoaded) {
            return;
        }
        saved = state.cached; // at most the catalog cache's cap (16)
    }
    std::vector<std::string> listed;
    listed.reserve(projects.size());
    for (const auto& p : projects) {
        listed.push_back(detail::AllProjectKey(p, backendKind));
    }
    const std::vector<std::string> keys =
        detail::RecentSeedKeys(std::move(saved), backendKind, endpoint, listed, smatchet::recents::kMaxPerScope);
    state.recentsSeedScope = scope;
    if (keys.empty() || !smatchet::recents::SharedGet(scope).empty()) {
        return;
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) { // oldest first: the newest ends on top
        smatchet::recents::RecordAndPersist(scope, *it);
    }
}

} // namespace

void StartAllProjectsFetch(State& state, AppController& app) {
    const std::string backendKey = app.FocusedCacheBackendKey();
    const bool offline = app.IsTrackerOffline();
    {
        std::lock_guard<std::mutex> lk(state.fetchMutex);
        const bool requested = state.fetchInFlight.load() || state.fetchDone.load();
        if (requested && state.fetchBackendKey != backendKey) {
            // The focused tracker changed since this list was requested: never show one tracker's
            // projects under another. A load still running for the old tracker is superseded (the
            // generation bump discards its result) and the focused tracker's list loads now.
            ++state.fetchGeneration;
            state.fetchedAll.clear();
            state.fetchError.clear();
            state.fetchFailed = false;
            state.fetchFromSaved = false;
            state.fetchSkippedOffline = false;
            state.fetchInFlight.store(false);
            state.fetchDone.store(false);
        } else if (state.fetchInFlight.load()) {
            return;
        } else if (state.fetchDone.load()) {
            if (!(state.fetchSkippedOffline && !offline)) {
                return; // loaded; a list skipped offline reloads once the tracker is reachable
            }
            state.fetchDone.store(false);
        }
    }
    // Strong handle to the active backend: the off-thread load captures it, so a live tracker swap
    // that frees AppController::Backend cannot dangle the client mid-listing (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = app.BackendShared();
    if (!backend) {
        return;
    }
    const std::shared_ptr<ILookupCache> store = app.LookupCacheShared();
    // Retry / Refresh ask the tracker even while a saved list is inside its lifetime (ttl 0 = always live).
    const bool bypassSaved = state.fetchBypassSaved.exchange(false);
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lk(state.fetchMutex);
        state.fetchBackendKey = backendKey;
        generation = ++state.fetchGeneration;
        state.fetchInFlight.store(true);
    }
    // `statePtr` is app-lifetime window state (heap-owned by the caller's draw session), safe to hold
    // as a raw pointer. The load runs on the app-owned joined pool (never a detached thread).
    State* statePtr = &state;
    try {
        app.LaunchBackgroundTask([statePtr, backend, store, backendKey, offline, generation, bypassSaved]() {
            // Settle the flags on every exit, so a throw can never leave the list on "Loading". A
            // superseded load leaves them alone: they belong to the load that replaced it.
            smatchet::ScopeExit settle([statePtr, generation]() {
                std::lock_guard<std::mutex> lk(statePtr->fetchMutex);
                if (statePtr->fetchGeneration == generation) {
                    statePtr->fetchDone.store(true);
                    statePtr->fetchInFlight.store(false);
                }
            });
            smatchet::projects::ProjectListOutcome outcome =
                smatchet::projects::LoadProjectList(backend->Connectivity(), store, backendKey, offline,
                                                    bypassSaved ? 0 : ConfigManager::Load().LookupCacheTtlSec);
            std::lock_guard<std::mutex> lk(statePtr->fetchMutex);
            if (statePtr->fetchGeneration != generation) {
                return; // superseded (the focused tracker changed): another tracker's list, dropped
            }
            // A failed retry with nothing saved keeps the rows already shown (now marked stale):
            // a failure never wipes a list the user has.
            if (!outcome.Failed || outcome.FromSaved || statePtr->fetchedAll.empty()) {
                statePtr->fetchedAll = std::move(outcome.Projects);
                // A list served from the lookup lifetime is a saved copy too: it never reads as live.
                statePtr->fetchFromSaved = smatchet::projects::IsSavedProjectCopy(outcome);
            }
            statePtr->fetchFailed = outcome.Failed;
            statePtr->fetchSkippedOffline = offline;
            statePtr->fetchError = outcome.Failed ? outcome.Error.Detail : std::string();
        });
    } catch (const std::exception& ex) {
        // The load never started: show the failure and a Retry instead of loading forever.
        LOG_WARN("SmatchetProjectPicker: loading the project list did not start: %s", ex.what());
        std::lock_guard<std::mutex> lk(state.fetchMutex);
        state.fetchFailed = true;
        state.fetchError = ex.what();
        state.fetchDone.store(true);
        state.fetchInFlight.store(false);
    }
}

void StartCachedProjectsLoad(State& state, AppController& app) {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lk(state.cachedMutex);
        generation = ++state.cachedGeneration;
    }
    // `statePtr` is app-lifetime window state, as for StartAllProjectsFetch.
    State* statePtr = &state;
    try {
        app.LaunchBackgroundTask([statePtr, generation]() {
            // Logs and returns an empty list on any failure; it never throws.
            std::vector<FieldCatalogCache::CachedProjectEntry> rows = FieldCatalogCache::ListCachedProjects();
            std::lock_guard<std::mutex> lk(statePtr->cachedMutex);
            if (statePtr->cachedGeneration != generation) {
                return; // a newer read (the popup reopened) owns the rows
            }
            statePtr->cached = std::move(rows);
            statePtr->cachedLoaded = true;
        });
    } catch (const std::exception& ex) {
        // The rows already shown stay; the next open reads again.
        LOG_WARN("SmatchetProjectPicker: reading the saved-catalog projects did not start: %s", ex.what());
    }
}

bool Draw(const char* idScope, State& state, AppController& app, const std::string& backendKind,
          const std::string& endpoint, std::string& selectedKey) {
    ImGui::PushID(idScope);
    bool changed = false;

    const char* placeholder = SmatchetLocalization::T("draft.project.placeholder", "(pick one)");
    const std::string preview = selectedKey.empty() ? std::string(placeholder) : selectedKey;

    if (ImGui::BeginCombo("##projectpicker", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
        if (!state.popupWasOpen) {
            StartCachedProjectsLoad(state, app); // once per open, never per frame (Pillar 2)
        }
        state.popupWasOpen = true;
        StartAllProjectsFetch(state, app); // returns at once when the list is loaded or loading

        const ProjectListView view = DrawProjectListStatus(state, app);
        const std::vector<ProjectRow> rows = BuildProjectRows(state, view.Projects, backendKind, endpoint);
        std::vector<smatchet::dropdown::Item> items;
        items.reserve(rows.size());
        for (const auto& row : rows) {
            smatchet::dropdown::Item item;
            item.Id = row.Key.c_str();
            item.Label = row.Label.c_str();
            item.Checked = row.Key == selectedKey;
            items.push_back(item);
        }
        // The last picks come from the shared store, per tracker instance, and are listed even when
        // the project list has not loaded (offline, first open): a recent project is always one pick away.
        const std::string backendKey = app.FocusedCacheBackendKey();
        const std::string recentsScope = smatchet::recents::MakeScopeKey(backendKey, "project");
        SeedRecentsFromSavedCatalogs(state, backendKey, view.Projects, backendKind, endpoint);
        smatchet::dropdown::Options options = smatchet::dropdown::SingleSelect(recentsScope);
        options.RecentsMinOptions = -1; // always: the last picks are the point, however short the list
        options.RecentsShowUnknown = true;
        // "No projects found." only ever describes a live, empty list; otherwise the cue above explains.
        options.EmptyText = view.Freshness == smatchet::offline::DataFreshness::Fresh
                                ? SmatchetLocalization::T("draft.project.none", "No projects found.")
                                : "";
        const smatchet::dropdown::Event ev = smatchet::dropdown::DrawBody(items, options);
        if (ev.Kind == smatchet::dropdown::EventKind::Picked && !ev.Id.empty()) {
            selectedKey = ev.Id;
            changed = true;
        }
        ImGui::EndCombo();
    } else {
        state.popupWasOpen = false;
    }

    ImGui::PopID();
    return changed;
}

} // namespace SmatchetProjectPicker
