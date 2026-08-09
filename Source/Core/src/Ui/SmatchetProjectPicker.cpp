#include "SmatchetProjectPicker.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "ITrackerBackend.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include "SmatchetProjectPicker_detail.h"
#include "Tracker/TrackerBackendKind.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
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
bool DrawRecentSection(const std::string& backendKind, const std::string& endpoint, const std::string& filter,
                       std::string& selectedKey) {
    bool changed = false;
    ImGui::Separator();
    ImGui::TextDisabled("%s", SmatchetLocalization::T("draft.project.section.recent", "Recently used"));
    std::vector<FieldCatalogCache::CachedProjectEntry> cached = FieldCatalogCache::ListCachedProjects();
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

// "All projects" collapsible section of the combo popup: kicks the lazy off-thread fetch on first
// expand, shows a loading state, then renders filtered rows. Owns its own TreeNodeEx/TreePop pair.
// Sets selectedKey + returns true when a row is picked. Runs inside the active BeginCombo scope.
bool DrawAllProjectsSection(State& state, AppController& app, const std::string& backendKind,
                            const std::shared_ptr<ITrackerBackend>& backend, const std::string& filter,
                            std::string& selectedKey) {
    bool changed = false;
    ImGui::Separator();
    const char* allLabel = SmatchetLocalization::T("draft.project.section.all", "All projects");
    if (ImGui::TreeNodeEx(allLabel, state.allExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        state.allExpanded = true;

        // Kick the lazy fetch on first expand. Routed through the app-owned joined
        // background-task pool (not a raw detached thread — forbidden by the
        // no-detach lint) so it is joined at shutdown. The lambda captures the
        // `backend` shared_ptr (a strong copy), so a live tracker swap that frees
        // AppController::Backend cannot dangle the client mid-ListProjects() — the
        // old backend stays alive until this task drops its copy (ADR 0012).
        // `statePtr` is app-lifetime window state (heap-owned by the caller's draw
        // session), safe to hold as a raw pointer.
        if (!state.fetchDone.load() && !state.fetchInFlight.load() && backend != nullptr) {
            state.fetchInFlight.store(true);
            State* statePtr = &state;
            app.LaunchBackgroundTask([statePtr, backend]() {
                std::vector<RemoteProject> projects;
                std::string err;
                try {
                    projects = backend->Connectivity().ListProjects();
                } catch (const std::exception& ex) {
                    err = ex.what();
                } catch (...) {
                    err = "unknown exception";
                }
                {
                    std::lock_guard<std::mutex> lk(statePtr->fetchMutex);
                    statePtr->fetchedAll = std::move(projects);
                    statePtr->fetchError = std::move(err);
                }
                statePtr->fetchDone.store(true);
                statePtr->fetchInFlight.store(false);
            });
        }

        if (state.fetchInFlight.load() && !state.fetchDone.load()) {
            ImGui::TextDisabled("  %s", SmatchetLocalization::T("draft.project.loading", "Loading..."));
        } else {
            std::vector<RemoteProject> snapshot;
            std::string fetchError;
            {
                std::lock_guard<std::mutex> lk(state.fetchMutex);
                snapshot = state.fetchedAll;
                fetchError = state.fetchError;
            }
            // P2-M11: the worker stored the failure but nothing rendered it — a bad or
            // expired token made the picker permanently, inexplicably empty for the
            // session (fetchDone latches). Show the error and let Retry re-kick.
            if (!fetchError.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, SmatchetTheme::GetActiveSemanticColors().ErrorText);
                ImGui::TextWrapped("%s",
                                   SmatchetLocalization::Format("draft.project.fetch_failed",
                                                                "Couldn't load projects: %s", fetchError.c_str()));
                ImGui::PopStyleColor();
                if (ImGui::SmallButton(SmatchetLocalization::T("draft.project.retry", "Retry"))) {
                    state.fetchDone.store(false); // next frame re-kicks the fetch
                }
            }
            int allShown = 0;
            for (const auto& p : snapshot) {
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
            if (allShown == 0 && fetchError.empty()) {
                ImGui::TextDisabled("  %s", filter.empty()
                                                ? SmatchetLocalization::T("draft.project.none", "No projects found.")
                                                : SmatchetLocalization::T("draft.project.none_filtered",
                                                                          "No projects match the filter."));
            }
        }
        ImGui::TreePop();
    } else {
        state.allExpanded = false;
    }
    return changed;
}

} // namespace

bool Draw(const char* idScope, State& state, AppController& app, const std::string& backendKind,
          const std::string& endpoint, std::string& selectedKey) {
    ImGui::PushID(idScope);
    bool changed = false;
    // Strong handle to the active backend (kept as a local arity to preserve the function key).
    // The OFF-THREAD fetch captures this shared_ptr so it survives a live tracker swap (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = app.BackendShared();

    const char* placeholder = SmatchetLocalization::T("draft.project.placeholder", "(pick one)");
    const std::string preview = selectedKey.empty() ? std::string(placeholder) : selectedKey;

    if (ImGui::BeginCombo("##projectpicker", preview.c_str())) {
        // Search field at top.
        const char* searchPlaceholder = SmatchetLocalization::T("draft.project.search", "Search...");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##projsearch", searchPlaceholder, state.searchBuf, sizeof(state.searchBuf));
        const std::string filter(state.searchBuf);

        // --- Recently used (always populated from cache; no async needed). ---
        if (DrawRecentSection(backendKind, endpoint, filter, selectedKey)) {
            changed = true;
        }

        // --- All projects (collapsible, lazy). ---
        if (DrawAllProjectsSection(state, app, backendKind, backend, filter, selectedKey)) {
            changed = true;
        }

        ImGui::EndCombo();
    }

    ImGui::PopID();
    return changed;
}

} // namespace SmatchetProjectPicker
