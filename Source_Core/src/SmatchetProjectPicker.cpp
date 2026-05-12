#include "SmatchetProjectPicker.h"

#include "FieldCatalogCache.h"
#include "ITrackerClient.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "StringUtil.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool ContainsCi(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return ToLowerAsciiCopy(haystack).find(ToLowerAsciiCopy(needle)) != std::string::npos;
}

std::string MakeRowLabel(const RemoteProject& p, const std::string& backendKind) {
    // Jira: "KEY — DisplayName". Plane: key is empty, display name only.
    if (backendKind == "Plane" || p.key.empty()) {
        return p.displayName.empty() ? p.id : p.displayName;
    }
    if (p.displayName.empty()) {
        return p.key;
    }
    return p.key + " \xE2\x80\x94 " + p.displayName;
}

} // namespace

namespace SmatchetProjectPicker {

bool Draw(const char* idScope, State& state, ITrackerClient* client, const std::string& backendKind,
          const std::string& endpoint, std::string& selectedKey) {
    ImGui::PushID(idScope);
    bool changed = false;

    const char* placeholder = SmatchetLocalization::T("draft.project.placeholder", "(pick one)");
    const std::string preview = selectedKey.empty() ? std::string(placeholder) : selectedKey;

    if (ImGui::BeginCombo("##projectpicker", preview.c_str())) {
        // Search field at top.
        const char* searchPlaceholder = SmatchetLocalization::T("draft.project.search", "Search...");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##projsearch", searchPlaceholder, state.searchBuf, sizeof(state.searchBuf));
        const std::string filter(state.searchBuf);

        // --- Recently used (always populated from cache; no async needed). ---
        ImGui::Separator();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("draft.project.section.recent", "Recently used"));
        std::vector<FieldCatalogCache::CachedProjectEntry> cached = FieldCatalogCache::ListCachedProjects();
        int recentShown = 0;
        for (const auto& e : cached) {
            if (e.projectKey.empty()) {
                continue;
            }
            if (!backendKind.empty() && !e.backend.empty() && e.backend != backendKind) {
                continue;
            }
            if (!endpoint.empty() && !e.endpoint.empty() && e.endpoint != endpoint) {
                continue;
            }
            if (!filter.empty() && !ContainsCi(e.projectKey, filter)) {
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
            ImGui::TextDisabled("  -");
        }

        // --- All projects (collapsible, lazy). ---
        ImGui::Separator();
        const char* allLabel =
            SmatchetLocalization::T("draft.project.section.all", "All projects");
        if (ImGui::TreeNodeEx(allLabel, state.allExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            state.allExpanded = true;

            // Kick the lazy fetch on first expand.
            if (!state.fetchDone.load() && !state.fetchInFlight.load() && client != nullptr) {
                state.fetchInFlight.store(true);
                State* statePtr = &state;
                ITrackerClient* clientPtr = client;
                std::thread([statePtr, clientPtr]() {
                    std::vector<RemoteProject> projects;
                    std::string err;
                    try {
                        projects = clientPtr->ListProjects();
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
                }).detach();
            }

            if (state.fetchInFlight.load() && !state.fetchDone.load()) {
                ImGui::TextDisabled("  %s",
                                    SmatchetLocalization::T("draft.project.loading", "Loading..."));
            } else {
                std::vector<RemoteProject> snapshot;
                {
                    std::lock_guard<std::mutex> lk(state.fetchMutex);
                    snapshot = state.fetchedAll;
                }
                int allShown = 0;
                for (const auto& p : snapshot) {
                    const std::string key = (backendKind == "Plane") ? p.id : p.key;
                    if (key.empty()) {
                        continue;
                    }
                    if (!filter.empty() && !ContainsCi(key, filter) && !ContainsCi(p.displayName, filter)) {
                        continue;
                    }
                    const std::string label = MakeRowLabel(p, backendKind);
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
                if (allShown == 0) {
                    ImGui::TextDisabled("  -");
                }
            }
            ImGui::TreePop();
        } else {
            state.allExpanded = false;
        }

        ImGui::EndCombo();
    }

    ImGui::PopID();
    return changed;
}

} // namespace SmatchetProjectPicker
