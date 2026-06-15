#include "SmatchetUI.h"

#include "GridPane.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "UiPerfMonitor.h"

#include "IconsFontAwesome6.h"

#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar (reserves a viewport side-bar work-area strip)
#include "SmatchetLocalizedImGui.h"

#include <string>

// Global Chrome-omnibox-style search bar (jql-omnibox plan, Stream B). Slice 2b draws the
// bar + wires the apply path; slice 2c adds the Jql|TicketKey|TitleSearch classifier. The
// bar reserves a top viewport side-bar so the docked grid windows shrink beneath it, and
// reuses the dashboard's JQL editor machinery (TrackerQueryAcp_*) on its OWN JqlEditorState
// instance (d.omniJqlEditor) — separate in-flight autocomplete request-ids, no cross-surface
// collision (slice 2a request-id isolation).

void SmatchetUI::drawOmnibar(AppController& app, UiDrawSession& d) {
    SMATCHET_UI_PERF_SCOPE("SmatchetUI::drawOmnibar");
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return;
    }

    // One frame-line of input + the window padding above/below it. Mechanism A: reserve via
    // BeginViewportSideBar(Up) from inside Draw (no host/frame-loop edit). If the host's
    // DockSpaceOverViewport (built before Draw) under-reserves and the grid overlaps the bar,
    // the plan's fallback B moves the reservation to the host seam (routed to ui-host).
    const float barHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
    const ImGuiWindowFlags barFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginViewportSideBar("##SmatchetOmnibar", viewport, ImGuiDir_Up, barHeight, barFlags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_MAGNIFYING_GLASS); // falls back to text if the FA glyph is absent
        ImGui::SameLine();
        // No project pill on the omnibar — the pill is hard-bound to the dashboard editor.
        SmatchetViewsDashboardUiDetail::DrawJqlQueryEditorEmbedded(app, d, d.omniJqlEditor,
                                                                   /*drawProjectPill=*/false);

        if (d.omniJqlEditor.jqlWantsApplyFromEnter) {
            d.omniJqlEditor.jqlWantsApplyFromEnter = false;
            const std::string query(d.omniJqlEditor.buf);
            if (!query.empty()) {
                // v1: treat the input as raw JQL/filter and drive the focused pane's view.
                // Slice 2c branches here on OmnibarInputClassifier (ticket-key jump / title search).
                switch (applyQueryToPaneView(app, d, d.focusedPane(), query)) {
                case ApplyQueryResult::Ok:
                    break; // happy path — the grid re-runs; no toast needed.
                case ApplyQueryResult::ViewUnavailable:
                    SmatchetToastManager::Instance().Push(
                        "Search", "No active view to search — open a grid pane first.", ToastType::Warning);
                    break;
                case ApplyQueryResult::UpdateFailed:
                    SmatchetToastManager::Instance().Push("Search", "Could not apply the query.", ToastType::Warning);
                    break;
                }
            }
        }
    }
    ImGui::End();
}

// Replaces the view owned by `target` with `query` and re-runs it. Adopts that view's
// identity first (without a network re-fetch — the UpdateActive + SyncWithCurrentView below
// kicks the sync) when it isn't already the active view. Mirrors the applied query into
// d.cfg.JqlQuery and the dashboard JQL editor buffer so every surface stays in lock-step.
// Shared core of userInfoAddToQuery (User-Info "add to query") and drawOmnibar.
SmatchetUI::ApplyQueryResult SmatchetUI::applyQueryToPaneView(AppController& app, UiDrawSession& d, GridPane& target,
                                                              const std::string& query) {
    const ViewDefinition* active = ViewState.GetActiveView();
    if (active == nullptr || active->Id != target.viewId) {
        viewsActivateView(app, d, target.viewId, /*kickSync=*/false);
        active = ViewState.GetActiveView();
    }
    if (active == nullptr || active->Id != target.viewId) {
        // Activation fell back to a non-target view (or none) — never overwrite an unrelated
        // view's query.
        return ApplyQueryResult::ViewUnavailable;
    }
    ViewDefinition updated = *active;
    updated.Jql = query;
    if (!ViewState.UpdateActive(updated)) {
        return ApplyQueryResult::UpdateFailed;
    }
    d.cfg.JqlQuery = query;
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlEditor.buf, query);
    SyncWithCurrentView(app, d, ViewState.GetStore(), true);
    return ApplyQueryResult::Ok;
}
