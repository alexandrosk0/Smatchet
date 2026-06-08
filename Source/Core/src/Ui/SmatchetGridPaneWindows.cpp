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
    // Bootstrap restore is a host-side focus assignment: replay it as a real focus
    // switch so the restored focused pane's saved viewId is ACTIVATED on frame 1
    // (review MEDIUM-3 — without this, startup rendered whatever view happened to
    // be active and the steady-state sync rebound the pane to it).
    d.gridPaneFocusReassigned = true;
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

bool ApplyPaneAddAndCloseRequests(UiDrawSession& d) {
    // Pure core (SmatchetGridPaneWindows_detail.cpp — bucket-A covered); the wrapper
    // only forwards the focus-reassignment report into the session latch the host
    // consumes next frame as a real focus switch (review HIGH-2).
    const detail::PaneRequestApplyOutcome outcome =
        detail::ApplyPaneAddAndCloseRequestsCore(d.gridPanes, d.focusedPaneId, d.paneAddRequestSourceId);
    if (outcome.FocusReassigned) {
        d.gridPaneFocusReassigned = true;
    }
    return outcome.Changed;
}

} // namespace SmatchetGridPaneWindows

// SmatchetUI members below — need ViewState / gridFrameCtx_ / drawActiveProjectWindow.

namespace {

/// RAII: routes shared grid helpers (header toolbar, cell support, new-issue
/// draft) to THIS pane via UiDrawSession::pane() for the duration of one
/// drawActiveProjectWindow call; resets on every exit path.
struct ScopedActivePaneForDraw {
    UiDrawSession& d;
    ScopedActivePaneForDraw(UiDrawSession& session, GridPane& pane) : d(session) { d.activePaneForDraw = &pane; }
    ~ScopedActivePaneForDraw() { d.activePaneForDraw = nullptr; }
};

} // namespace

void SmatchetUI::drawGridPaneWindows(AppController& app, UiDrawSession& d) {
    SmatchetGridPaneWindows::EnsurePanesLoaded(d);

    // Once-per-frame services that lived in the single-grid window before Slice 2
    // (running them per pane would double background work).
    annotateAnalysisUi_.SetAnnotatePanelOpen(d.showAnnotateAnalysis);
    annotateAnalysisUi_.ServiceBackground();

    // Connectivity banner resolved ONCE per frame for every pane + the edit pump
    // below (was per pane — N visible panes paid N probes per frame).
    const TrackerConnectivityBannerForUi trackerBanner = app.GetTrackerConnectivityBannerForUi(nullptr);

    // One dockable window per pane. The vector is stable across the loop — add/close
    // requests are deferred to ApplyPaneAddAndCloseRequests below.
    d.paneWindowFocusedThisFrame.clear();
    for (size_t i = 0; i < d.gridPanes.size(); ++i) {
        SMATCHET_UI_PERF_SCOPE("pane.render");
        GridPane& pane = d.gridPanes[i];
        pane.focused = (pane.id == d.focusedPaneId);
        ScopedActivePaneForDraw activePane(d, pane);
        drawActiveProjectWindow(app, d, pane, trackerBanner);
    }

    // Focus switch: the pane whose window holds ImGui focus becomes the focused pane —
    // global actions + the AppController focused-context delegators follow it (every
    // visible pane is live since Slice 3; focus only selects the delegator target).
    // A host-side reassignment from last
    // frame (focused-pane close / "+" duplicate / bootstrap restore) replays as a
    // real switch via d.gridPaneFocusReassigned so the new focused pane's saved
    // view gets activated (review HIGH-2 / MEDIUM-3 — without it, focusSwitched
    // stayed false and the steady-state sync rebound the survivor to the CLOSED
    // pane's still-active view, then persisted the loss).
    const bool windowFocusMoved =
        !d.paneWindowFocusedThisFrame.empty() && d.paneWindowFocusedThisFrame != d.focusedPaneId;
    const bool focusSwitched = windowFocusMoved || d.gridPaneFocusReassigned;
    d.gridPaneFocusReassigned = false; // consume-once
    if (windowFocusMoved) {
        SMATCHET_UI_PERF_SCOPE("pane.switch");
        d.focusedPaneId = d.paneWindowFocusedThisFrame;
        SmatchetGridPaneWindows::MarkPanesDirty(d);
    }
    // Multi-grid Slice 3: AppController's focused-context delegators follow the UI's
    // focused pane (cheap early-out when unchanged). Must land BEFORE the focus-switch
    // sync below so SyncWithCurrentView targets the newly focused pane's OWN context.
    app.SetFocusedPane(d.focusedPaneId);
    if (GridPane* focused = FindGridPaneById(d.gridPanes, d.focusedPaneId)) {
        syncFocusedPaneWithActiveView(app, d, *focused, focusSwitched);
    }

    // Deferred toolbar actions ({paneId, kind} latch — review MEDIUM-2, extended to
    // "+ New Issue" by plan item 19): a click in a not-yet-focused pane is applied
    // HERE, after the focus/view switch above landed, so the action targets the
    // clicked pane's view/context — not the previously focused pane's. Consume-once:
    // a request whose pane did not gain focus is dropped, never replayed.
    if (!d.paneDeferredActionPaneId.empty()) {
        if (d.paneDeferredActionPaneId == d.focusedPaneId) {
            if (const ViewDefinition* active = ViewState.GetActiveView()) {
                if (d.paneDeferredActionKind == UiDrawSession::PaneDeferredActionKind::RefreshView) {
                    d.cfg.JqlQuery = active->Jql;
                    d.cfg.SelectedFields = active->Fields;
                    SyncWithCurrentView(app, d, ViewState.GetStore(), true);
                } else if (d.paneDeferredActionKind == UiDrawSession::PaneDeferredActionKind::NewIssueDraft) {
                    static const std::vector<CachedTicket> kNoTickets;
                    const GridPane& focusedPane = d.focusedPane();
                    StartNewIssueDraft(app, d, ViewState.GetActiveViewMutable(),
                                       focusedPane.ticketsSnapshot ? *focusedPane.ticketsSnapshot : kNoTickets);
                }
            }
        }
        d.paneDeferredActionPaneId.clear();
        d.paneDeferredActionKind = UiDrawSession::PaneDeferredActionKind::None;
    }

    // Field-edit dispatch pump + chip decay ONCE per frame (review MEDIUM-1): panes
    // only ENQUEUE (EnqueueGridFieldEdits). A per-pane pump faded success chips N×
    // faster and could dequeue an edit during a non-focused pane's call, snapshotting
    // estimate bases from that pane's FROZEN ticket snapshot. The pump reads the
    // focused pane's live snapshot.
    {
        const bool readOnlyMode =
            d.cfg.ReadOnlyMode || (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Error);
        static const std::vector<CachedTicket> kNoTickets;
        const GridPane& focused = d.focusedPane();
        const std::vector<CachedTicket>& pumpTickets = focused.ticketsSnapshot ? *focused.ticketsSnapshot : kNoTickets;
        PumpGridFieldEdits(app, d, pumpTickets, readOnlyMode);
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
            // pane's backend; the SESSION-level lastViewsBackendKey delta (consumed
            // in drawViewStateAndConnectivity — review HIGH-4) resets catalog +
            // initial sync next frame, and the sync path performs the actual swap.
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
        // A `pane.rename`-pinned custom label stops following the active view name
        // (multi-grid Slice 4). viewId/backendKey below still track the live context.
        if (!pane.titleOverridden && pane.title != nowActive->Name) {
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
