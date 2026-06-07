// SmatchetGridPaneWindows — dockable grid-pane window host (multi-grid-tabs
// Slice 2, plan item 15, ADR-0018). Owns: pane bootstrap from smatchet_panes.json,
// the once-per-pane window loop over the re-entrant drawActiveProjectWindow,
// focus tracking (which pane drives the single Slice-2 live context), the "+"
// duplicate / close-X requests (applied after the loop; min 1 pane survives),
// and the debounced panes-file save. Stacking/splitting is NATIVE ImGui docking
// — no bespoke tab bar or splitter; dock geometry rides ImGui's .ini.

#include "SmatchetUI.h"
#include "SmatchetGridPaneWindows.h"
#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <chrono>
#include <string>
#include <vector>

namespace SmatchetGridPaneWindows {

void EnsurePanesLoaded(UiDrawSession& d) {
    if (d.gridPanesLoaded || !d.cfgInitialized) {
        return;
    }
    const PersistentPanesFile disk = ConfigManager::LoadPanesOrBootstrap(d.cfg);
    // Discard any frame-1 self-healed placeholder — nothing meaningful lives in a
    // pane's runtime state before the first pane-window draw.
    d.gridPanes.clear();
    d.gridPanes.reserve(disk.Panes.size());
    for (const auto& desc : disk.Panes) {
        GridPane pane;
        pane.id = desc.Id;
        pane.title = desc.Title;
        pane.backendKey = desc.BackendKey;
        pane.viewId = desc.ViewId;
        d.gridPanes.push_back(pane);
    }
    d.focusedPaneId = disk.FocusedPaneId;
    if (d.gridPanes.empty()) {
        // LoadPanesOrBootstrap never returns an empty list, but stay crash-proof.
        GridPane fallback;
        fallback.id = "main";
        fallback.title = "Grid";
        d.gridPanes.push_back(fallback);
    }
    if (FindGridPaneById(d.gridPanes, d.focusedPaneId) == nullptr) {
        d.focusedPaneId = d.gridPanes.front().id;
    }
    d.gridPanesLoaded = true;
    LOG_INFO("GridPaneWindows: loaded %zu pane(s), focused='%s'", d.gridPanes.size(), d.focusedPaneId.c_str());
}

void PersistPanes(const UiDrawSession& d) {
    PersistentPanesFile disk;
    disk.FocusedPaneId = d.focusedPaneId;
    disk.Panes.reserve(d.gridPanes.size());
    for (const auto& pane : d.gridPanes) {
        GridPaneDescriptor desc;
        desc.Id = pane.id;
        desc.Title = pane.title;
        desc.BackendKey = pane.backendKey;
        desc.ViewId = pane.viewId;
        disk.Panes.push_back(desc);
    }
    ConfigManager::SavePanesToDisk(disk);
}

void MarkPanesDirty(UiDrawSession& d) {
    if (!d.gridPanesDirty) {
        d.gridPanesDirty = true;
        d.gridPanesSaveDueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    }
}

void DrainPanesSaveIfDue(UiDrawSession& d) {
    if (d.gridPanesDirty && std::chrono::steady_clock::now() >= d.gridPanesSaveDueAt) {
        d.gridPanesDirty = false;
        PersistPanes(d);
    }
}

std::string GenerateUniquePaneId(const std::vector<GridPane>& panes) {
    for (int n = 2;; ++n) {
        const std::string candidate = "pane-" + std::to_string(n);
        bool taken = false;
        for (const auto& pane : panes) {
            if (pane.id == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
    }
}

bool ApplyPaneAddAndCloseRequests(UiDrawSession& d) {
    bool changed = false;

    // Close sweep — windows whose tab X was clicked wrote pane.open = false.
    // Min-1 invariant: never let the sweep empty the vector.
    for (size_t i = 0; i < d.gridPanes.size();) {
        if (!d.gridPanes[i].open && d.gridPanes.size() > 1) {
            const bool wasFocused = (d.gridPanes[i].id == d.focusedPaneId);
            d.gridPanes.erase(d.gridPanes.begin() + static_cast<std::ptrdiff_t>(i));
            if (wasFocused) {
                d.focusedPaneId = d.gridPanes.front().id;
            }
            changed = true;
        } else {
            d.gridPanes[i].open = true; // re-arm the survivor (covers the last-pane case)
            ++i;
        }
    }

    // "+" request — duplicate the source pane (same backend + view; the new pane
    // shares the source's snapshot pointer, so opening costs no ticket copy).
    if (!d.paneAddRequestSourceId.empty()) {
        const GridPane* src = FindGridPaneById(d.gridPanes, d.paneAddRequestSourceId);
        if (src == nullptr) {
            src = &d.gridPanes.front();
        }
        GridPane dup;
        dup.id = GenerateUniquePaneId(d.gridPanes);
        dup.title = src->title;
        dup.backendKey = src->backendKey;
        dup.viewId = src->viewId;
        dup.ticketsSnapshot = src->ticketsSnapshot;
        dup.snapshotRevision = src->snapshotRevision;
        d.gridPanes.push_back(dup); // invalidates `src` — done reading it above
        d.focusedPaneId = d.gridPanes.back().id;
        d.paneAddRequestSourceId.clear();
        changed = true;
    }

    return changed;
}

} // namespace SmatchetGridPaneWindows

// ---------------------------------------------------------------------------
// SmatchetUI members — need ViewState / gridFrameCtx_ / drawActiveProjectWindow.
// ---------------------------------------------------------------------------

void SmatchetUI::drawGridPaneWindows(AppController& app, UiDrawSession& d) {
    SmatchetGridPaneWindows::EnsurePanesLoaded(d);

    // Once-per-frame services that lived in the single-grid window before Slice 2
    // (running them per pane would double background work).
    annotateAnalysisUi_.SetAnnotatePanelOpen(d.showAnnotateAnalysis);
    annotateAnalysisUi_.ServiceBackground();

    // One dockable window per pane. The vector is stable across the loop — add/close
    // requests are deferred to ApplyPaneAddAndCloseRequests below.
    d.paneWindowFocusedThisFrame.clear();
    for (size_t i = 0; i < d.gridPanes.size(); ++i) {
        SMATCHET_UI_PERF_SCOPE("pane.render");
        GridPane& pane = d.gridPanes[i];
        pane.focused = (pane.id == d.focusedPaneId);
        // RAII-light: activePaneForDraw routes shared grid helpers (header toolbar,
        // cell support, new-issue draft) to THIS pane for the duration of the call.
        d.activePaneForDraw = &pane;
        drawActiveProjectWindow(app, d, pane);
        d.activePaneForDraw = nullptr;
    }

    // Focus switch: the pane whose window holds ImGui focus becomes the focused pane
    // and takes over the single Slice-2 live context (Slice 3 makes every visible
    // pane live and deletes this hand-over).
    const bool focusSwitched = !d.paneWindowFocusedThisFrame.empty() && d.paneWindowFocusedThisFrame != d.focusedPaneId;
    if (focusSwitched) {
        SMATCHET_UI_PERF_SCOPE("pane.switch");
        d.focusedPaneId = d.paneWindowFocusedThisFrame;
        SmatchetGridPaneWindows::MarkPanesDirty(d);
    }
    if (GridPane* focused = FindGridPaneById(d.gridPanes, d.focusedPaneId)) {
        syncFocusedPaneWithActiveView(app, d, *focused, focusSwitched);
    }

    if (SmatchetGridPaneWindows::ApplyPaneAddAndCloseRequests(d)) {
        SmatchetGridPaneWindows::MarkPanesDirty(d);
    }
    SmatchetGridPaneWindows::DrainPanesSaveIfDue(d);
}

// Focused-pane <-> active-view reconciliation. Two directions:
//   * focus SWITCH (pane drives): activate the pane's saved view; if the pane points
//     at a different backend, update cfg and let the EXISTING sync chokepoint
//     (SyncWithCurrentView -> TicketSyncService::SwapBackendIfTrackerChanged) perform
//     the per-context backend swap — the grid never talks to Jira/Plane clients
//     directly (plan item 16).
//   * steady state (view drives): the focused pane FOLLOWS the active view, so header
//     combo / Views-dashboard switches update the pane identity + window title.
void SmatchetUI::syncFocusedPaneWithActiveView(AppController& app, UiDrawSession& d, GridPane& pane,
                                               bool focusSwitched) {
    if (focusSwitched) {
        const std::string cfgKey = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
        if (!pane.backendKey.empty() && pane.backendKey != cfgKey) {
            // SLICE-2 BOUNDARY: one live GridLiveContext. Re-point the config at the
            // pane's backend; the per-pane lastViewsBackendKey delta (consumed in
            // drawViewStateAndConnectivity) resets catalog + initial sync next frame,
            // and the sync path performs the actual client swap.
            d.cfg.TrackerType = pane.backendKey;
            ConfigManager::Save(d.cfg);
            ViewState.EnsureLoaded(d.cfg);
            LOG_INFO("GridPaneWindows: focused pane '%s' re-pointed backend to '%s'", pane.id.c_str(),
                     pane.backendKey.c_str());
        }
        const ViewDefinition* active = ViewState.GetActiveView();
        if (!pane.viewId.empty() && (active == nullptr || active->Id != pane.viewId)) {
            // Views::Activate(pane.viewId) + JQL/fields adoption + SyncWithCurrentView
            // (which routes through SwapBackendIfTrackerChanged) — the existing path.
            viewsActivateView(app, d, pane.viewId);
        }
    }

    // Steady state: keep pane identity in lockstep with the active view + backend.
    const ViewDefinition* nowActive = ViewState.GetActiveView();
    if (nowActive != nullptr) {
        bool identityChanged = false;
        if (pane.viewId != nowActive->Id) {
            pane.viewId = nowActive->Id;
            identityChanged = true;
        }
        if (pane.title != nowActive->Name) {
            pane.title = nowActive->Name;
            identityChanged = true;
        }
        const std::string cfgKey = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
        if (pane.backendKey != cfgKey) {
            pane.backendKey = cfgKey;
            identityChanged = true;
        }
        if (identityChanged) {
            SmatchetGridPaneWindows::MarkPanesDirty(d);
        }
    }
}
