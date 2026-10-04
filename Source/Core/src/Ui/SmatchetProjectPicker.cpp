#include "SmatchetProjectPicker.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "DataFreshnessCue.h"
#include "FieldCatalogCache.h"
#include "ITrackerBackend.h"
#include "Logger.h"
#include "OfflineFirstPure.h"
#include "ProjectListLookup.h"
#include "ScopeExit.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include "SmatchetProjectPicker_detail.h"
#include "Tracker/TrackerBackendKind.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
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
    if (smatchet::tracker::IsPlaneBackendType(cfg.TrackerType)) {
        outBackendKind = "Plane";
        outEndpoint = cfg.PlaneUrl + "|" + cfg.PlaneWorkspaceSlug;
        return;
    }
    if (smatchet::tracker::BackendIndexFromType(cfg.TrackerType) == smatchet::tracker::kBackendLinear) {
        outBackendKind = "Linear";
        outEndpoint = cfg.LinearBaseUrl + "|" + cfg.LinearTeamId;
        return;
    }
    outBackendKind = "Jira";
    outEndpoint = cfg.Domain;
}

namespace {

// "Recently used" section of the combo popup: cache-backed rows filtered by backend/endpoint/text.
// Sets selectedKey + returns true when a row is picked (and closes the popup, matching the
// pre-decomposition body). Runs inside the active BeginCombo scope.
bool DrawRecentSection(const std::vector<FieldCatalogCache::CachedProjectEntry>& cached, const std::string& backendKind,
                       const std::string& endpoint, const std::string& filter, std::string& selectedKey) {
    bool changed = false;
    ImGui::Separator();
    ImGui::TextDisabled("%s", SmatchetLocalization::T("draft.project.section.recent", "Recently used"));
    int recentShown = 0;
    for (const auto& e : cached) {
        if (!detail::RecentEntryPasses(e, backendKind, endpoint, filter)) {
            continue;
        }
        const bool selected = (e.projectKey == selectedKey);
        ImGui::PushID(static_cast<int>(recentShown));
        if (ImGui::Selectable(e.projectKey.c_str(), selected)) {
            selectedKey = e.projectKey;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        ++recentShown;
    }
    if (recentShown == 0) {
        ImGui::TextDisabled("  %s", SmatchetLocalization::T("draft.project.recent.none", "No recent projects"));
    }
    return changed;
}

// Rows of the "All projects" list that pass the filter; sets selectedKey + returns true when one is
// picked. `freshness` decides whether an empty list reads "No projects found." (only a live list does).
bool DrawAllProjectRows(const std::vector<RemoteProject>& projects, const std::string& backendKind,
                        const std::string& filter, smatchet::offline::DataFreshness freshness,
                        std::string& selectedKey) {
    bool changed = false;
    int allShown = 0;
    for (const auto& p : projects) {
        const std::string key = detail::AllProjectKey(p, backendKind);
        if (!detail::AllProjectPasses(key, p, filter)) {
            continue;
        }
        const std::string label = detail::MakeRowLabel(p, backendKind);
        const bool selected = (key == selectedKey);
        ImGui::PushID(static_cast<int>(allShown));
        if (ImGui::Selectable(label.c_str(), selected)) {
            selectedKey = key;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        ++allShown;
    }
    if (allShown == 0 && !filter.empty()) {
        ImGui::TextDisabled("  %s",
                            SmatchetLocalization::T("draft.project.none_filtered", "No projects match the filter."));
    } else if (allShown == 0 && freshness == smatchet::offline::DataFreshness::Fresh) {
        ImGui::TextDisabled("  %s", SmatchetLocalization::T("draft.project.none", "No projects found."));
    }
    return changed;
}

// "All projects" collapsible section of the combo popup: kicks the lazy off-thread load on first
// expand, then renders the live or saved rows under a freshness cue. Owns its own TreeNodeEx/TreePop
// pair. Sets selectedKey + returns true when a row is picked. Runs inside the active BeginCombo scope.
bool DrawAllProjectsSection(State& state, AppController& app, const std::string& backendKind, const std::string& filter,
                            std::string& selectedKey) {
    bool changed = false;
    ImGui::Separator();
    const char* allLabel = SmatchetLocalization::T("draft.project.section.all", "All projects");
    if (ImGui::TreeNodeEx(allLabel, state.allExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        state.allExpanded = true;
        StartAllProjectsFetch(state, app);

        std::vector<RemoteProject> snapshot;
        std::string fetchError;
        bool failed = false;
        bool fromSaved = false;
        {
            std::lock_guard<std::mutex> lk(state.fetchMutex);
            snapshot = state.fetchedAll;
            fetchError = state.fetchError;
            failed = state.fetchFailed;
            fromSaved = state.fetchFromSaved;
        }
        const bool inFlight = state.fetchInFlight.load();
        const bool done = state.fetchDone.load();
        smatchet::offline::FreshnessInputs in;
        in.HasCache = !snapshot.empty() || fromSaved || (done && !failed);
        in.Live = done && !failed && !fromSaved;
        in.InFlight = inFlight;
        in.LastAttemptFailed = failed;
        in.Connectivity = app.GetLastTrackerConnectivityState();
        const smatchet::offline::DataFreshness freshness = smatchet::offline::ClassifyFreshness(in);
        const bool showRows = smatchet::offline::ShouldRenderContent(freshness);
        // Pillar 6: saved or stale rows stay visible under the cue; the failure is its tooltip.
        DataFreshnessCue::Draw(freshness, fetchError.empty() ? nullptr : fetchError.c_str());
        if (failed && !inFlight) {
            // P2-M11: with nothing to show, the error itself is the content — a bad or expired token
            // must never read as an empty project list. Retry re-kicks the load next frame.
            if (!showRows && !fetchError.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, SmatchetTheme::GetActiveSemanticColors().ErrorText);
                ImGui::TextWrapped("%s",
                                   SmatchetLocalization::Format("draft.project.fetch_failed",
                                                                "Couldn't load projects: %s", fetchError.c_str()));
                ImGui::PopStyleColor();
            }
            if (ImGui::SmallButton(SmatchetLocalization::T("draft.project.retry", "Retry"))) {
                state.fetchDone.store(false);
            }
        }
        if (showRows && DrawAllProjectRows(snapshot, backendKind, filter, freshness, selectedKey)) {
            changed = true;
        }
        ImGui::TreePop();
    } else {
        state.allExpanded = false;
    }
    return changed;
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
        app.LaunchBackgroundTask([statePtr, backend, store, backendKey, offline, generation]() {
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
                smatchet::projects::LoadProjectList(backend->Connectivity(), store, backendKey, offline);
            std::lock_guard<std::mutex> lk(statePtr->fetchMutex);
            if (statePtr->fetchGeneration != generation) {
                return; // superseded (the focused tracker changed): another tracker's list, dropped
            }
            // A failed retry with nothing saved keeps the rows already shown (now marked stale):
            // a failure never wipes a list the user has.
            if (!outcome.Failed || outcome.FromSaved || statePtr->fetchedAll.empty()) {
                statePtr->fetchedAll = std::move(outcome.Projects);
                statePtr->fetchFromSaved = outcome.FromSaved;
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

bool Draw(const char* idScope, State& state, AppController& app, const std::string& backendKind,
          const std::string& endpoint, std::string& selectedKey) {
    ImGui::PushID(idScope);
    bool changed = false;

    const char* placeholder = SmatchetLocalization::T("draft.project.placeholder", "(pick one)");
    const std::string preview = selectedKey.empty() ? std::string(placeholder) : selectedKey;

    const bool open = ImGui::BeginCombo("##projectpicker", preview.c_str());
    // Pillar 2: the catalog-cache index is a file read + JSON parse, so it is read once per open.
    switch (detail::RecentSnapshotActionFor(open, state.comboWasOpen)) {
    case detail::RecentSnapshotAction::Refresh:
        state.recentSnapshot = FieldCatalogCache::ListCachedProjects();
        break;
    case detail::RecentSnapshotAction::Release:
        std::vector<FieldCatalogCache::CachedProjectEntry>().swap(state.recentSnapshot);
        break;
    case detail::RecentSnapshotAction::Keep:
        break;
    }
    if (open) {
        // Search field at top.
        const char* searchPlaceholder = SmatchetLocalization::T("draft.project.search", "Search...");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##projsearch", searchPlaceholder, state.searchBuf, sizeof(state.searchBuf));
        const std::string filter(state.searchBuf);

        // --- Recently used (the cache snapshot taken when the combo opened). ---
        if (DrawRecentSection(state.recentSnapshot, backendKind, endpoint, filter, selectedKey)) {
            changed = true;
        }

        // --- All projects (collapsible, lazy). ---
        if (DrawAllProjectsSection(state, app, backendKind, filter, selectedKey)) {
            changed = true;
        }

        ImGui::EndCombo();
    }

    ImGui::PopID();
    return changed;
}

} // namespace SmatchetProjectPicker
