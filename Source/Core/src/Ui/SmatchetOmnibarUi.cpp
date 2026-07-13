#include "SmatchetUI.h"

#include "AppController.h" // OpenUrl / BuildIssueBrowseUrl (ticket-key jump)
#include "GridPane.h"
#include "OmnibarInputClassifier.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "UiPerfMonitor.h"

#include "IconsFontAwesome6.h"

#include "imgui.h"
#include "imgui_internal.h" // BeginViewportSideBar (reserves a viewport side-bar work-area strip)
#include "SmatchetLocalizedImGui.h"

#include <memory>
#include <string>
#include <vector>

// Global Chrome-omnibox-style search bar (jql-omnibox plan, Stream B). Slice 2b drew the bar +
// wired the apply path; slice 2c adds the Jql|TicketKey|TitleSearch classifier so Enter does the
// right thing for what was typed: a bare key jumps to the ticket, a structured query replaces the
// focused view's query, plain words filter the focused grid. The leading glyph previews that
// decision every frame. The bar reserves a top viewport side-bar so the docked grid windows shrink
// beneath it, and reuses the dashboard's JQL editor machinery (TrackerQueryAcp_*) on its OWN
// JqlEditorState instance (d.omniJqlEditor) — separate in-flight autocomplete request-ids, no
// cross-surface collision (slice 2a request-id isolation).

namespace omni = smatchet::omnibar;

namespace {

// Leading mode glyph for the omnibar, picked from the live classification so the affordance always
// previews what Enter will do: # jump-to-ticket / funnel structured-filter / lens title-search.
const char* OmnibarModeGlyph(omni::OmnibarInputKind kind) {
    switch (kind) {
    case omni::OmnibarInputKind::TicketKey:
        return ICON_FA_HASHTAG;
    case omni::OmnibarInputKind::Jql:
        return ICON_FA_FILTER;
    case omni::OmnibarInputKind::TitleSearch:
    default:
        return ICON_FA_MAGNIFYING_GLASS;
    }
}

// Hover hint paired with the glyph — spells out the Enter action in words.
const char* OmnibarModeTooltip(omni::OmnibarInputKind kind) {
    switch (kind) {
    case omni::OmnibarInputKind::TicketKey:
        return SmatchetLocalization::T("omnibar.mode.ticket_key", "Ticket key — Enter opens this issue.");
    case omni::OmnibarInputKind::Jql:
        return SmatchetLocalization::T("omnibar.mode.jql", "Filter query — Enter replaces the focused view's query.");
    case omni::OmnibarInputKind::TitleSearch:
    default:
        return SmatchetLocalization::T("omnibar.mode.title_search", "Title search — Enter filters the focused grid.");
    }
}

// Ticket-key Enter (sync v1): jump in-grid when the row is already loaded in the focused pane's
// snapshot (no network), otherwise open the backend browse URL in the user's browser. Snapshot
// membership only — an async existence-fetch for not-yet-loaded keys is a deferred follow-up.
void OmnibarJumpToTicket(AppController& app, UiDrawSession& d, GridPane& target, const std::string& key) {
    const std::shared_ptr<const std::vector<CachedTicket>>& rows = target.ticketsSnapshot;
    if (rows) {
        for (const CachedTicket& ticket : *rows) {
            if (ticket.id == key) {
                target.gridState.SetActiveIssue(key);
                return;
            }
        }
    }
    app.OpenUrl(app.BuildIssueBrowseUrl(d.cfg, key));
}

} // namespace

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
        GridPane& pane = d.focusedPane();

        // Open-frame focus grab: on the frame the omnibar window first appears, request keyboard
        // focus for the input so the very first keystroke lands (no 1-frame-stale focus). The flag
        // is consumed by DrawJqlQueryEditorEmbedded's SetKeyboardFocusHere on THIS same frame
        // (it is checked before the InputText submit below), so focus is granted on the open-edge
        // rather than a frame later.
        if (ImGui::IsWindowAppearing()) {
            d.omniJqlEditor.jqlAcpWantsJqlInputFocus = true;
        }

        // Per-tab persistence: each pane keeps its own omnibar text. On a focus change, load the
        // newly-focused pane's saved text into the shared editor; edits are written back at the end
        // of the frame so switching tabs and returning restores what was typed for that tab. The
        // editor's transient autocomplete state recomputes from buf each frame, so a text-only swap
        // is safe (the popup is closed on a focus change anyway).
        if (d.omnibarSyncedPaneId != pane.id) {
            SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.omniJqlEditor.buf, pane.omnibarText);
            d.omnibarSyncedPaneId = pane.id;
        }

        const omni::OmnibarBackend backend = omni::OmnibarBackendFromKey(pane.backendKey);

        // Classify the live buffer every frame (pure, allocation-free) to drive the leading glyph
        // so the user can see whether Enter will jump / filter / search before committing.
        const omni::OmnibarInputKind liveKind = omni::ClassifyOmnibarInput(d.omniJqlEditor.buf, backend);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(OmnibarModeGlyph(liveKind)); // falls back to text if the FA glyph is absent
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", OmnibarModeTooltip(liveKind));
        }
        ImGui::SameLine();
        // No project pill on the omnibar — the pill is hard-bound to the dashboard editor.
        // The placeholder distinguishes this ISSUE-search bar from the command-search
        // entry points (menu-bar box / Ctrl+Shift+P palette).
        SmatchetViewsDashboardUiDetail::DrawJqlQueryEditorEmbedded(
            app, d, d.omniJqlEditor,
            /*drawProjectPill=*/false,
            SmatchetLocalization::T("omnibar.hint", "Search issues: key, filter query, or title text"));

        if (d.omniJqlEditor.jqlWantsApplyFromEnter) {
            d.omniJqlEditor.jqlWantsApplyFromEnter = false;
            applyOmnibarEnter(app, d, pane, std::string(d.omniJqlEditor.buf));
        }

        // Write this frame's edits back to the focused pane so the text persists when the user
        // switches tabs (the restore above reloads it when this pane regains focus). Only assign
        // when the buffer actually differs — the bar redraws every frame, so an unconditional
        // assign() copies (and can reallocate) the std::string each frame even when nothing was
        // typed. The char-array compare is allocation-free and skips the write in the steady state.
        if (pane.omnibarText != d.omniJqlEditor.buf) {
            pane.omnibarText.assign(d.omniJqlEditor.buf);
        }
    }
    ImGui::End();
}

// Routes a committed omnibar Enter against `target` per OmnibarInputClassifier (slice 2c, sync v1).
// Trims once, then dispatches on the classified kind: TicketKey jumps the pane, TitleSearch fills
// its grid filter box, Jql drives applyQueryToPaneView (the shared view-apply core). Empty input is
// a no-op so Enter on a blank bar never wipes the grid filter.
void SmatchetUI::applyOmnibarEnter(AppController& app, UiDrawSession& d, GridPane& target, const std::string& raw) {
    const std::string input = TrimCopyAsciiWhitespace(raw);
    if (input.empty()) {
        return;
    }
    const omni::OmnibarBackend backend = omni::OmnibarBackendFromKey(target.backendKey);
    switch (omni::ClassifyOmnibarInput(input, backend)) {
    case omni::OmnibarInputKind::TicketKey:
        OmnibarJumpToTicket(app, d, target, input);
        return;
    case omni::OmnibarInputKind::TitleSearch:
        SmatchetViewsDashboardUiDetail::CopyStringToBuffer(target.gridFilterBuf, input);
        return;
    case omni::OmnibarInputKind::Jql:
    default:
        break;
    }
    switch (applyQueryToPaneView(app, d, target, input)) {
    case ApplyQueryResult::Ok:
        break; // happy path — the grid re-runs; no toast needed.
    case ApplyQueryResult::ViewUnavailable:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.search", "Search"),
            SmatchetLocalization::T("omnibar.no_active_view", "No active view to search — open a grid pane first."),
            ToastType::Warning);
        break;
    case ApplyQueryResult::UpdateFailed:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.search", "Search"),
            SmatchetLocalization::T("omnibar.apply_failed", "Could not apply the query."), ToastType::Warning);
        break;
    }
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
    SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
    return ApplyQueryResult::Ok;
}
