#include "SmatchetGridUiSupport.h"
#include "SmatchetGridHeaderUi_detail.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"
#include "SmatchetTheme.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "Views.h"
#include "IssueDraft.h"
#include "ProjectResolver.h"
#include "Ui/SmatchetIconButtons.h"
#include "SmatchetImGuiFonts.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {
constexpr std::chrono::seconds kHeaderChipFadeOutDuration(5);

#if defined(SMATCHET_WITH_MCP)
/** After "TRACKER OK" auto-hides (~10s), keep MCP LIVE visible this much longer at launch. */
constexpr auto kMcpHeaderExtraAfterTrackerOkHidden = std::chrono::seconds(10);
/** When tracker is not in OK auto-hide mode, still show MCP LIVE this long from enable anchor. */
constexpr auto kMcpHeaderInitialVisibleFallback = std::chrono::seconds(20);
constexpr auto kMcpHeaderIdleHideAfter = std::chrono::milliseconds(2500);
constexpr auto kMcpHeaderFlickerRecent = std::chrono::milliseconds(350);
#endif

ImVec4 LerpTowardBackground(const ImVec4& fg, const ImVec4& bg, float fade01) {
    const float t = std::min(1.0f, std::max(0.0f, fade01));
    return ImVec4(fg.x + (bg.x - fg.x) * t, fg.y + (bg.y - fg.y) * t, fg.z + (bg.z - fg.z) * t,
                  fg.w + (bg.w - fg.w) * t);
}

using SmatchetGridHeader::detail::ChipFadeFraction;

// Holds the resolved right-side header chip widths and flags computed before layout.
struct GridHeaderRightChips {
    const char* trLabel = "";
    ImVec4 trColor{};
    std::string trTip;
    bool showTrackerChip = false;
    float trackerChipFade01 = 0.0f;
    float trW = 0.0f;

    bool showMcpHeaderChip = false;
    float mcpChipFade01 = 0.0f;
    float mcpW = 0.0f;
    std::string mcpLabelStr;

    const char* readOnlyLabel = "READ ONLY";
    bool showReadOnlyChip = false;
    float readOnlyW = 0.0f;

    float btnW = 0.0f;
    float between = 0.0f;
    ImVec4 headerBgCol{};
};

// Map the chip-state struct onto the pure width-accumulator inputs and run it.
float AccumulateRightWidth(const GridHeaderRightChips& c, bool readOnlyMode) {
    SmatchetGridHeader::detail::RightClusterWidths w;
    w.showTrackerChip = c.showTrackerChip;
    w.trackerW = c.trW;
    w.showReadOnlyChip = c.showReadOnlyChip;
    w.readOnlyW = c.readOnlyW;
    w.showMcpChip = c.showMcpHeaderChip;
    w.mcpW = c.mcpW;
    w.newIssueBtnW = c.btnW;
    w.betweenSpacing = c.between;
    return SmatchetGridHeader::detail::AccumulateRightWidth(w, readOnlyMode);
}

// Body of the "Sort By" popup: per-rule direction combos, delete buttons, add-rule menu.
void DrawSortByPopupBody(UiDrawSession& d, ViewDefinition*& activeViewForGrid,
                         const std::vector<TicketGridColumn>& columns) {
    ImGui::TextDisabled("Active Sort Rules");
    bool sortChanged = false;

    for (size_t i = 0; i < activeViewForGrid->SortSpecs.size();) {
        ViewSortSpec& spec = activeViewForGrid->SortSpecs[i];
        ImGui::PushID(static_cast<int>(i));

        std::string colName = spec.ColumnKey;
        auto colIt =
            std::find_if(columns.begin(), columns.end(), [&](const auto& c) { return c.Key == spec.ColumnKey; });
        if (colIt != columns.end()) {
            colName = colIt->Label;
        }

        const bool removeSortClicked = ImGui::Button("X");
        ImGui::SetItemTooltip("%s", SmatchetLocalization::T("grid.sort.remove_key", "Remove this sort key"));
        if (removeSortClicked) {
            SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *activeViewForGrid);
            activeViewForGrid->SortSpecs.erase(activeViewForGrid->SortSpecs.begin() + i);
            sortChanged = true;
            ImGui::PopID();
            continue; // Do not increment i
        }
        ImGui::SameLine();

        const char* dirStr = (spec.Direction == 1) ? "Ascending" : "Descending";
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("##dir", dirStr, ImGuiComboFlags_NoArrowButton)) {
            if (ImGui::Selectable("Ascending", spec.Direction == 1)) {
                SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *activeViewForGrid);
                spec.Direction = 1;
                sortChanged = true;
            }
            if (ImGui::Selectable("Descending", spec.Direction == 2)) {
                SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *activeViewForGrid);
                spec.Direction = 2;
                sortChanged = true;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Text("%s", colName.c_str());

        ImGui::PopID();
        ++i;
    }

    if (activeViewForGrid->SortSpecs.empty()) {
        ImGui::TextDisabled("  No sort active.");
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("+ Add Sort Rule")) {
        for (const auto& c : columns) {
            bool alreadySorted = std::any_of(activeViewForGrid->SortSpecs.begin(), activeViewForGrid->SortSpecs.end(),
                                             [&](const auto& s) { return s.ColumnKey == c.Key; });
            if (!alreadySorted && ImGui::MenuItem(c.Label.c_str())) {
                SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *activeViewForGrid);
                ViewSortSpec newSpec;
                newSpec.ColumnKey = c.Key;
                newSpec.Direction = 1; // Default Ascending
                activeViewForGrid->SortSpecs.push_back(newSpec);
                sortChanged = true;
            }
        }
        ImGui::EndMenu();
    }

    if (sortChanged) {
        d.viewSortDirty = true;
        d.pane().forceApplySortSpecs = true; // per-pane since Slice 2
        d.viewsDirty = true;
    }
}

// Left toolbar: view selector combo, refresh button, quick filter, sort-by popup.
void DrawHeaderViewToolbar(AppController& app, UiDrawSession& d, ViewDefinition*& activeViewForGrid,
                           const std::vector<TicketGridColumn>& columns, Views& viewState) {
    // Live-focus gate context (review MEDIUM-2): pane.focused is LAST frame's host
    // verdict. Single-click actions ("Refresh View") fired from a not-yet-focused
    // pane must defer one frame so the host applies the focus/view switch first.
    // Multi-frame widgets (view combo, Sort By popup) self-heal: their opening click
    // moves window focus, so the mutating click lands on a post-switch frame where
    // this pane's view IS the active view.
    GridPane& pane = d.pane();
    ImGui::Separator();
    // Header view-identity text (Slice 4 fallback-leak fix): for a non-owned pane,
    // activeViewForGrid is the FOCUSED view standing in as a render fallback (cross-backend
    // unfocused pane whose own bucket isn't loaded). Showing its Name leaked the focused
    // view's identity into the wrong pane's header. Strict-Id ownership (same discipline as
    // the columns/widths/sort gates): when not the pane's own, show the pane's OWN persisted
    // view name (pane.title) and never highlight a focused-bucket view as this pane's.
    const bool headerViewIsPanesOwn = activeViewForGrid && activeViewForGrid->Id == pane.viewId;
    const char* headerViewLabel = headerViewIsPanesOwn
                                      ? activeViewForGrid->Name.c_str()
                                      : (!pane.title.empty() ? pane.title.c_str() : "(no active view)");
    // View Dropdown Selector (replaces old plain text label)
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##ViewSelectorCombo", headerViewLabel)) {
        for (const auto& view : viewState.GetStore().Views) {
            const bool isSelected = headerViewIsPanesOwn && (activeViewForGrid->Id == view.Id);
            if (ImGui::Selectable(view.Name.c_str(), isSelected)) {
                if (viewState.Activate(view.Id)) {
                    activeViewForGrid = viewState.GetActiveViewMutable();
                    if (activeViewForGrid) {
                        d.cfg.JqlQuery = activeViewForGrid->Jql;
                        d.cfg.SelectedFields = activeViewForGrid->Fields;
                        SyncWithCurrentView(app, d, viewState.GetStore(), true);
                    }
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Switch active grid view");
    }

    // Refresh View — re-runs the active view's query and refreshes the grid.
    // Sort/layout edits surface in the unsaved-layout strip below this toolbar; no
    // separate green "Save View" chip is needed.
    if (activeViewForGrid) {
        ImGui::SameLine();
        if (SmatchetIconButton(ICON_FA_ARROWS_ROTATE, "Refresh View",
                               "Re-run this view's query and refresh the grid.")) {
            if (pane.focused) {
                d.cfg.JqlQuery = activeViewForGrid->Jql;
                d.cfg.SelectedFields = activeViewForGrid->Fields;
                SyncWithCurrentView(app, d, viewState.GetStore(), true);
            } else {
                // Not-yet-focused pane (review MEDIUM-2): syncing NOW would re-run a
                // query against the still-focused pane's live context. Defer — the
                // host consumes this after applying the focus/view switch this click
                // triggered (drawGridPaneWindows; consume-once).
                d.paneDeferredActionPaneId = pane.id;
                d.paneDeferredActionKind = UiDrawSession::PaneDeferredActionKind::RefreshView;
            }
        }
    }

    // Quick Filter UI — per-pane buffer (Slice 2): each pane window filters alone.
    ImGui::SameLine(0, 30.0f);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputTextWithHint("##GridFilter", "Filter...", pane.gridFilterBuf, sizeof(pane.gridFilterBuf))) {
        // Filter changed
    }
    if (pane.gridFilterBuf[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            pane.gridFilterBuf[0] = '\0';
        }
    }

    // Modern Sort By Popup UX
    if (activeViewForGrid) {
        ImGui::SameLine(0, 30.0f);
        if (ImGui::Button("Sort By \xE2\x86\x95")) {
            ImGui::OpenPopup("SortByPopup");
        }
        if (ImGui::BeginPopup("SortByPopup")) {
            DrawSortByPopupBody(d, activeViewForGrid, columns);
            ImGui::EndPopup();
        }
    }
}

// Resolve the tracker connectivity chip label/color/tip from banner + connectivity state.
void ResolveTrackerChip(AppController& app, const TrackerConnectivityBannerForUi& trackerBanner,
                        GridHeaderRightChips& c) {
    if (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
        c.trLabel = "● TRACKER ERROR";
        c.trColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        c.trTip = trackerBanner.Message;
    } else if (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Warning) {
        c.trLabel = "● TRACKER OFFLINE";
        c.trColor = ImVec4(1.0f, 0.82f, 0.22f, 1.0f);
        c.trTip = trackerBanner.Message;
    } else {
        const AppController::TrackerConnectivityState connSt = app.GetLastTrackerConnectivityState();
        switch (connSt) {
        case AppController::TrackerConnectivityState::Unknown:
            c.trLabel = "● TRACKER …";
            c.trColor = ImVec4(0.55f, 0.58f, 0.62f, 1.0f);
            c.trTip = "Checking tracker reachability.";
            break;
        case AppController::TrackerConnectivityState::AuthenticatedReachable:
            c.trLabel = "● TRACKER OK";
            c.trColor = ImVec4(0.2f, 0.82f, 0.38f, 1.0f);
            c.trTip = "Live tracker connection.";
            break;
        case AppController::TrackerConnectivityState::ReachableAuthOrConfigError:
            c.trLabel = "● TRACKER AUTH";
            c.trColor = ImVec4(1.0f, 0.72f, 0.28f, 1.0f);
            c.trTip = "Host answered; fix credentials or Preferences → Tracker.";
            break;
        case AppController::TrackerConnectivityState::TransportDown:
        case AppController::TrackerConnectivityState::ServiceUnavailable:
            c.trLabel = "● TRACKER DOWN";
            c.trColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
            {
                const std::string& tw = app.GetLastTicketSyncWarning();
                c.trTip = tw.empty() ? std::string("Tracker not reachable.") : tw;
            }
            break;
        default:
            c.trLabel = "● TRACKER";
            c.trColor = ImVec4(0.65f, 0.68f, 0.72f, 1.0f);
            c.trTip = "Tracker connectivity state is indeterminate.";
            break;
        }
    }
}

// Drive the tracker-OK chip auto-hide + fade timer state machine; sets show/fade on c.
void UpdateTrackerChipVisibility(AppController& app, UiDrawSession& d,
                                 const TrackerConnectivityBannerForUi& trackerBanner,
                                 std::chrono::steady_clock::time_point nowChip, GridHeaderRightChips& c) {
    const bool connectivityGoodOk =
        (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::None) &&
        (app.GetLastTrackerConnectivityState() == AppController::TrackerConnectivityState::AuthenticatedReachable);

    c.showTrackerChip = true;
    c.trackerChipFade01 = 0.0f;
    if (connectivityGoodOk) {
        if (!d.trackerOkChipHideTimerArmed) {
            d.trackerOkChipHideTimerArmed = true;
            d.trackerOkChipHideAt = nowChip + std::chrono::seconds(10);
        }
        if (nowChip < d.trackerOkChipHideAt) {
            c.showTrackerChip = true;
            c.trackerChipFade01 = 0.0f;
        } else if (nowChip < d.trackerOkChipHideAt + kHeaderChipFadeOutDuration) {
            c.showTrackerChip = true;
            c.trackerChipFade01 =
                ChipFadeFraction(std::chrono::duration<float>(nowChip - d.trackerOkChipHideAt).count(),
                                 std::chrono::duration<float>(kHeaderChipFadeOutDuration).count());
        } else {
            c.showTrackerChip = false;
            c.trackerChipFade01 = 1.0f;
        }
    } else {
        d.trackerOkChipHideTimerArmed = false;
        c.showTrackerChip = true;
        c.trackerChipFade01 = 0.0f;
    }

    c.trW = c.showTrackerChip ? ImGui::CalcTextSize(c.trLabel).x : 0.0f;
}

#if defined(SMATCHET_WITH_MCP)
// Drive the MCP LIVE chip visibility/fade state machine and resolve its label + width.
void UpdateMcpChip(AppController& app, UiDrawSession& d, const TrackerConnectivityBannerForUi& trackerBanner,
                   std::chrono::steady_clock::time_point nowChip, GridHeaderRightChips& c) {
    const bool connectivityGoodOk =
        (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::None) &&
        (app.GetLastTrackerConnectivityState() == AppController::TrackerConnectivityState::AuthenticatedReachable);

    const auto nowMcp = nowChip;
    if (d.cfg.McpEnabled != d.mcpLiveHeaderLastCfgEnabled) {
        d.mcpLiveHeaderLastCfgEnabled = d.cfg.McpEnabled;
        d.mcpLiveHeaderAnchorAt = nowMcp;
        d.mcpLiveHeaderAnchorArmed = true;
        d.mcpHeaderFadeoutActive = false;
        d.mcpHeaderLastFrameChipShown = false;
    } else if (!d.mcpLiveHeaderAnchorArmed) {
        d.mcpLiveHeaderAnchorAt = nowMcp;
        d.mcpLiveHeaderAnchorArmed = true;
    }
    const bool mcpChipWasDrawnLastFrame = d.mcpHeaderLastFrameChipShown;

    std::chrono::steady_clock::time_point lastMcp{};
    const bool haveLastMcp = d.cfg.McpEnabled && app.TryGetMcpLastClientHttpActivity(&lastMcp);

    bool withinInitial = false;
    if (connectivityGoodOk) {
        // Stay at least until online tracker chip hides, then longer (see kMcpHeaderExtraAfterTrackerOkHidden).
        withinInitial = nowMcp < (d.trackerOkChipHideAt + kMcpHeaderExtraAfterTrackerOkHidden);
    } else {
        withinInitial = (nowMcp - d.mcpLiveHeaderAnchorAt) < kMcpHeaderInitialVisibleFallback;
    }
    bool withinIdleWindow = false;
    if (haveLastMcp) {
        withinIdleWindow = (nowMcp - lastMcp) < kMcpHeaderIdleHideAfter;
    }
    const bool mcpLogicalVisibleEnabled = withinInitial || withinIdleWindow;

    c.showMcpHeaderChip = false;
    c.mcpChipFade01 = 0.0f;

    if (mcpLogicalVisibleEnabled) {
        d.mcpHeaderFadeoutActive = false;
        c.showMcpHeaderChip = true;
        c.mcpChipFade01 = 0.0f;
    } else {
        if (mcpChipWasDrawnLastFrame && !d.mcpHeaderFadeoutActive) {
            d.mcpHeaderFadeoutActive = true;
            d.mcpHeaderFadeoutStartAt = nowMcp;
        }
        if (d.mcpHeaderFadeoutActive) {
            const float fadeSec = std::chrono::duration<float>(nowMcp - d.mcpHeaderFadeoutStartAt).count();
            const float fadeDur = std::chrono::duration<float>(kHeaderChipFadeOutDuration).count();
            if (fadeSec < fadeDur) {
                c.showMcpHeaderChip = true;
                c.mcpChipFade01 = fadeSec / fadeDur;
            } else {
                c.showMcpHeaderChip = false;
                d.mcpHeaderFadeoutActive = false;
                c.mcpChipFade01 = 1.0f;
            }
        } else {
            c.showMcpHeaderChip = false;
        }
    }

    if (c.showMcpHeaderChip) {
        if (!d.cfg.McpEnabled) {
            c.mcpLabelStr = "● MCP DISABLED";
        } else if (d.cfg.McpAllowRemote) {
            c.mcpLabelStr = "● MCP LIVE: " + std::to_string(d.cfg.McpPort) + " (LAN)";
        } else {
            c.mcpLabelStr = "● MCP LIVE: " + std::to_string(d.cfg.McpPort);
        }
    }

    d.mcpHeaderLastFrameChipShown = c.showMcpHeaderChip;

    c.mcpW = c.showMcpHeaderChip ? ImGui::CalcTextSize(c.mcpLabelStr.c_str()).x : 0.0f;
}
#endif

} // namespace

// Begin a new-issue draft seeded from the last visible ticket (or last-ticket fallback). Also
// the entry point through which the pane-window host replays a deferred "+ New Issue" from a
// not-yet-focused pane after the focus/view switch lands (multi-grid Slice 3, plan item 19).
void StartNewIssueDraft(AppController& app, UiDrawSession& d, ViewDefinition* activeViewForGrid,
                        const std::vector<CachedTicket>& tickets) {
    if (!d.newIssueDraftActive) {
        const CachedTicket* lastVisibleTicket = nullptr;
        if (!tickets.empty()) {
            size_t lastIndex = tickets.size() - 1;
            if (!d.pane().filteredIndices.empty()) {
                lastIndex = d.pane().filteredIndices.back();
            }
            if (lastIndex < tickets.size()) {
                lastVisibleTicket = &tickets[lastIndex];
            }
        }

        const std::vector<std::string>& inheritIds =
            (d.cfg.TrackerType == "Plane")
                ? d.cfg.NewIssueInheritFieldIdsPlane
                : (d.cfg.TrackerType == "Linear") ? d.cfg.NewIssueInheritFieldIdsLinear : d.cfg.NewIssueInheritFieldIds;
        if (lastVisibleTicket) {
            const std::string activeViewQuery = activeViewForGrid ? activeViewForGrid->Jql : std::string();
            // No global cfg.ProjectKey exists — pass "" as the legacy fallback.
            const ITrackerBackend* gb = app.GetTrackerBackend();
            const std::string resolvedProject = smatchet::ResolveProjectForDraft(
                gb ? &gb->Connectivity() : nullptr, activeViewQuery, lastVisibleTicket->id, std::string());
            d.newIssueDraft =
                IssueDraftHelpers::FromCachedTicket(*lastVisibleTicket, app.GetAvailableFields(), resolvedProject,
                                                    d.cfg.DefaultIssueTypeId, d.cfg.DefaultIssueTypeName, inheritIds);
        } else {
            d.newIssueDraft = app.BuildDraftFromLastTicket(d.cfg);
        }
        if (!d.newIssueDraft.ParentKey.empty()) {
            d.newIssueDraft.FieldValues["parent"] = d.newIssueDraft.ParentKey;
        }
        d.newIssueDraftActive = true;
        d.newIssueDraftEditBufs.clear();
        d.newIssueMissingFieldIds.clear();
        d.newIssueQueueFallbackVisible = false;
        d.newIssueQueueFallbackError.clear();
        d.gridEditError.clear();
        d.gridEditSuccess.clear();
    }
    d.newIssueScrollDraftRowIntoViewPending = true;
    d.newIssueFocusSummaryPending = true;
}

namespace { // reopened — see the external-linkage note above StartNewIssueDraft

// Right-aligned cluster: tracker chip, READ ONLY chip, MCP chip, compose-new-issue icon button.
void DrawHeaderRightChips(AppController& app, UiDrawSession& d, ViewDefinition* activeViewForGrid,
                          const std::vector<CachedTicket>& tickets, bool readOnlyMode,
                          std::chrono::steady_clock::time_point nowChip, const GridHeaderRightChips& c) {
    const float totalRightW = AccumulateRightWidth(c, readOnlyMode);
    if (totalRightW <= 0.0f) {
        return;
    }

    ImGui::SameLine();
    const float targetX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - totalRightW;
    if (targetX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(targetX);
    }

    if (c.showTrackerChip) {
        const ImVec4 trDraw = LerpTowardBackground(c.trColor, c.headerBgCol, c.trackerChipFade01);
        ImGui::TextColored(trDraw, "%s", c.trLabel);
        if (!c.trTip.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", c.trTip.c_str());
        }
    }

    if (c.showReadOnlyChip) {
        if (c.showTrackerChip) {
            ImGui::SameLine(0.0f, c.between);
        }
        ImGui::TextColored(SmatchetTheme::GetActiveSemanticColors().WarningText, "%s", c.readOnlyLabel);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Read-only mode is enabled in Preferences. Tracker-changing actions are disabled.");
        }
    }
#if defined(SMATCHET_WITH_MCP)
    if ((c.showTrackerChip || c.showReadOnlyChip) && c.showMcpHeaderChip) {
        ImGui::SameLine(0.0f, c.between);
    }
    if (c.showMcpHeaderChip) {
        ImVec4 mcpBase = c.trColor;
        if (!d.cfg.McpEnabled) {
            mcpBase = ImVec4(1.0f, 0.8f, 0.25f, 1.0f);
        } else {
            std::chrono::steady_clock::time_point lastMcpDraw{};
            if (app.TryGetMcpLastClientHttpActivity(&lastMcpDraw) &&
                (nowChip - lastMcpDraw) < kMcpHeaderFlickerRecent) {
                const int phase = static_cast<int>(ImGui::GetTime() / 0.12) & 1;
                mcpBase = phase != 0 ? c.trColor : ImVec4(1.0f, 0.82f, 0.22f, 1.0f);
            }
        }
        const ImVec4 mcpDraw = LerpTowardBackground(mcpBase, c.headerBgCol, c.mcpChipFade01);
        ImGui::TextColored(mcpDraw, "%s", c.mcpLabelStr.c_str());
        if (ImGui::IsItemHovered()) {
            if (d.cfg.McpEnabled) {
                ImGui::SetTooltip("MCP on port %d. %s Auth: %s.", d.cfg.McpPort,
                                  d.cfg.McpAllowRemote ? "Bound on all interfaces." : "Localhost only.",
                                  d.cfg.McpAuthToken.empty() ? "loopback only (no token)."
                                                             : "X-Smatchet-Token required.");
            } else {
                ImGui::SetTooltip("MCP server is disabled. Enable it under Settings → Preferences → Integrations.");
            }
        }
    }
#else
    (void)nowChip;
#endif

    if (!readOnlyMode) {
        if (c.showTrackerChip || c.showReadOnlyChip || c.showMcpHeaderChip) {
            ImGui::SameLine(0.0f, c.between);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.58f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.38f, 0.78f, 1.0f));
        // Icon-only "compose new issue" button (FA pen-to-square). Falls back to the
        // "+ New Issue" text label when the FA font is missing and keeps the hover
        // tooltip in both paths (Pillar 4 — never a naked unlabelled glyph).
        if (SmatchetIconButton(ICON_FA_PEN_TO_SQUARE, "+ New Issue",
                               "Create a new issue draft and scroll grid to bottom")) {
            const GridPane& clickPane = d.pane();
            if (clickPane.focused) {
                StartNewIssueDraft(app, d, activeViewForGrid, tickets);
            } else {
                // Not-yet-focused pane (multi-grid Slice 3, plan item 19 — same shape as
                // the deferred Refresh View above): drafting NOW would seed from the
                // still-focused pane's active view/backend. Defer to the host, which
                // applies it after the focus/view switch this click triggered.
                d.paneDeferredActionPaneId = clickPane.id;
                d.paneDeferredActionKind = UiDrawSession::PaneDeferredActionKind::NewIssueDraft;
            }
        }
        ImGui::PopStyleColor(3);
    }
}
} // namespace

void DrawGridHeaderToolbar(AppController& app, UiDrawSession& d, ViewDefinition*& activeViewForGrid,
                           const std::vector<TicketGridColumn>& columns, const std::vector<CachedTicket>& tickets,
                           bool readOnlyMode, Views& viewState, const TrackerConnectivityBannerForUi& trackerBanner) {
    DrawHeaderViewToolbar(app, d, activeViewForGrid, columns, viewState);

    GridHeaderRightChips chips;
    ResolveTrackerChip(app, trackerBanner, chips);

    const auto nowChip = std::chrono::steady_clock::now();
    chips.headerBgCol = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);

    UpdateTrackerChipVisibility(app, d, trackerBanner, nowChip, chips);

#if defined(SMATCHET_WITH_MCP)
    UpdateMcpChip(app, d, trackerBanner, nowChip, chips);
#else
    chips.showMcpHeaderChip = false;
    chips.mcpW = 0.0f;
#endif

    chips.showReadOnlyChip = d.cfg.ReadOnlyMode;
    chips.readOnlyW = chips.showReadOnlyChip ? ImGui::CalcTextSize(chips.readOnlyLabel).x : 0.0f;

    // Mirror the icon-only button's rendered width: the FA glyph when the icon font
    // is loaded, else the "+ New Issue" text fallback (see DrawHeaderRightChips).
    const char* newIssueBtnVisible = SmatchetAreFaIconsLoaded() ? ICON_FA_PEN_TO_SQUARE : "+ New Issue";
    chips.btnW = ImGui::CalcTextSize(newIssueBtnVisible).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    chips.between = ImGui::GetStyle().ItemSpacing.x * 2.5f;

    DrawHeaderRightChips(app, d, activeViewForGrid, tickets, readOnlyMode, nowChip, chips);

    ImGui::Separator();
}
