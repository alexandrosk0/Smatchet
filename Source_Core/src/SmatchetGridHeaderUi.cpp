#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"
#include "SmatchetTheme.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "Views.h"
#include "IssueDraft.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
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
} // namespace

void DrawGridHeaderToolbar(AppController& app, UiDrawSession& d,
                           ViewDefinition*& activeViewForGrid,
                           const std::vector<TicketGridColumn>& columns,
                           const std::vector<CachedTicket>& tickets,
                           bool readOnlyMode,
                           Views& viewState,
                           const TrackerConnectivityBannerForUi& trackerBanner) {
    ImGui::Separator();
    // View Dropdown Selector (replaces old plain text label)
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##ViewSelectorCombo", activeViewForGrid ? activeViewForGrid->Name.c_str() : "(no active view)")) {
        for (const auto& view : viewState.GetStore().Views) {
            const bool isSelected = activeViewForGrid && (activeViewForGrid->Id == view.Id);
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

    // View Save/Refresh Buttons
    if (activeViewForGrid) {
        ImGui::SameLine();
        if (d.viewSortDirty) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.65f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.75f, 0.35f, 1.0f));
            if (ImGui::Button("Save View")) {
                viewState.Save();
                d.viewSortDirty = false;
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save sort order changes to this view.");
        } else {
            if (ImGui::Button("Refresh View")) {
                d.cfg.JqlQuery = activeViewForGrid->Jql;
                d.cfg.SelectedFields = activeViewForGrid->Fields;
                SyncWithCurrentView(app, d, viewState.GetStore(), true);
            }
        }
    }

    // Quick Filter UI
    ImGui::SameLine(0, 30.0f);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputTextWithHint("##GridFilter", "Filter...", d.gridFilterBuf, sizeof(d.gridFilterBuf))) {
        // Filter changed
    }
    if (d.gridFilterBuf[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) { d.gridFilterBuf[0] = '\0'; }
    }

    // Modern Sort By Popup UX
    if (activeViewForGrid) {
        ImGui::SameLine(0, 30.0f);
        if (ImGui::Button("Sort By \xE2\x86\x95")) {
            ImGui::OpenPopup("SortByPopup");
        }
        if (ImGui::BeginPopup("SortByPopup")) {
            ImGui::TextDisabled("Active Sort Rules");
            bool sortChanged = false;
            
            for (size_t i = 0; i < activeViewForGrid->SortSpecs.size(); ) {
                ViewSortSpec& spec = activeViewForGrid->SortSpecs[i];
                ImGui::PushID(static_cast<int>(i));
                
                std::string colName = spec.ColumnKey;
                auto colIt = std::find_if(columns.begin(), columns.end(), [&](const auto& c) {
                    return c.Key == spec.ColumnKey;
                });
                if (colIt != columns.end()) {
                    colName = colIt->Label;
                }
                
                if (ImGui::Button("X")) {
                    activeViewForGrid->SortSpecs.erase(activeViewForGrid->SortSpecs.begin() + i);
                    sortChanged = true;
                    ImGui::PopID();
                    continue; // Do not increment i
                }
                ImGui::SameLine();
                
                const char* dirStr = (spec.Direction == 1) ? "Ascending" : "Descending";
                ImGui::SetNextItemWidth(100.0f);
                if (ImGui::BeginCombo("##dir", dirStr, ImGuiComboFlags_NoArrowButton)) {
                    if (ImGui::Selectable("Ascending", spec.Direction == 1)) { spec.Direction = 1; sortChanged = true; }
                    if (ImGui::Selectable("Descending", spec.Direction == 2)) { spec.Direction = 2; sortChanged = true; }
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
                    bool alreadySorted = std::any_of(activeViewForGrid->SortSpecs.begin(), activeViewForGrid->SortSpecs.end(), [&](const auto& s) {
                        return s.ColumnKey == c.Key;
                    });
                    if (!alreadySorted && ImGui::MenuItem(c.Label.c_str())) {
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
                d.forceApplySortSpecs = true;
            }
            
            ImGui::EndPopup();
        }
    }

    const char* trLabel = "● TRACKER";
    ImVec4 trColor(0.65f, 0.68f, 0.72f, 1.0f);
    std::string trTip;
    if (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
        trLabel = "● TRACKER ERROR";
        trColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
        trTip = trackerBanner.Message;
    } else if (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::Warning) {
        trLabel = "● TRACKER OFFLINE";
        trColor = ImVec4(1.0f, 0.82f, 0.22f, 1.0f);
        trTip = trackerBanner.Message;
    } else {
        const AppController::TrackerConnectivityState connSt = app.GetLastTrackerConnectivityState();
        switch (connSt) {
        case AppController::TrackerConnectivityState::Unknown:
            trLabel = "● TRACKER …";
            trColor = ImVec4(0.55f, 0.58f, 0.62f, 1.0f);
            trTip = "Checking tracker reachability.";
            break;
        case AppController::TrackerConnectivityState::AuthenticatedReachable:
            trLabel = "● TRACKER OK";
            trColor = ImVec4(0.2f, 0.82f, 0.38f, 1.0f);
            trTip = "Live tracker connection.";
            break;
        case AppController::TrackerConnectivityState::ReachableAuthOrConfigError:
            trLabel = "● TRACKER AUTH";
            trColor = ImVec4(1.0f, 0.72f, 0.28f, 1.0f);
            trTip = "Host answered; fix credentials or Preferences → Tracker.";
            break;
        case AppController::TrackerConnectivityState::TransportDown:
        case AppController::TrackerConnectivityState::ServiceUnavailable:
            trLabel = "● TRACKER DOWN";
            trColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
            {
                const std::string& tw = app.GetLastTicketSyncWarning();
                trTip = tw.empty() ? std::string("Tracker not reachable.") : tw;
            }
            break;
        default:
            trTip = "Tracker connectivity state is indeterminate.";
            break;
        }
    }

    const bool connectivityGoodOk =
        (trackerBanner.Kind == TrackerConnectivityBannerForUi::Level::None) &&
        (app.GetLastTrackerConnectivityState() == AppController::TrackerConnectivityState::AuthenticatedReachable);
    const auto nowChip = std::chrono::steady_clock::now();
    const ImVec4 headerBgCol = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);

    bool showTrackerChip = true;
    float trackerChipFade01 = 0.0f;
    if (connectivityGoodOk) {
        if (!d.trackerOkChipHideTimerArmed) {
            d.trackerOkChipHideTimerArmed = true;
            d.trackerOkChipHideAt = nowChip + std::chrono::seconds(10);
        }
        if (nowChip < d.trackerOkChipHideAt) {
            showTrackerChip = true;
            trackerChipFade01 = 0.0f;
        } else if (nowChip < d.trackerOkChipHideAt + kHeaderChipFadeOutDuration) {
            showTrackerChip = true;
            trackerChipFade01 =
                std::chrono::duration<float>(nowChip - d.trackerOkChipHideAt).count()
                / std::chrono::duration<float>(kHeaderChipFadeOutDuration).count();
        } else {
            showTrackerChip = false;
            trackerChipFade01 = 1.0f;
        }
    } else {
        d.trackerOkChipHideTimerArmed = false;
        showTrackerChip = true;
        trackerChipFade01 = 0.0f;
    }

    const float trW = showTrackerChip ? ImGui::CalcTextSize(trLabel).x : 0.0f;

#if defined(SMATCHET_WITH_MCP)
    const auto nowMcp = nowChip;
    if (d.cfg.McpEnabled != d.mcpLiveHeaderLastCfgEnabled) {
        d.mcpLiveHeaderLastCfgEnabled = d.cfg.McpEnabled;
        if (d.cfg.McpEnabled) {
            d.mcpLiveHeaderAnchorAt = nowMcp;
        }
        d.mcpHeaderFadeoutActive = false;
        d.mcpHeaderLastFrameChipShown = false;
    }
    const bool mcpChipWasDrawnLastFrame = d.mcpHeaderLastFrameChipShown;

    std::string mcpLabelStr;
    bool showMcpHeaderChip = false;
    float mcpChipFade01 = 0.0f;
    bool mcpLogicalVisibleEnabled = false;

    if (!d.cfg.McpEnabled) {
        showMcpHeaderChip = true;
        mcpChipFade01 = 0.0f;
        mcpLabelStr = "● MCP DISABLED";
    } else {
        std::chrono::steady_clock::time_point lastMcp{};
        const bool haveLastMcp = app.TryGetMcpLastClientHttpActivity(&lastMcp);
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
        mcpLogicalVisibleEnabled = withinInitial || withinIdleWindow;

        if (mcpLogicalVisibleEnabled) {
            d.mcpHeaderFadeoutActive = false;
            showMcpHeaderChip = true;
            mcpChipFade01 = 0.0f;
        } else {
            if (mcpChipWasDrawnLastFrame && !d.mcpHeaderFadeoutActive) {
                d.mcpHeaderFadeoutActive = true;
                d.mcpHeaderFadeoutStartAt = nowMcp;
            }
            if (d.mcpHeaderFadeoutActive) {
                const float fadeSec =
                    std::chrono::duration<float>(nowMcp - d.mcpHeaderFadeoutStartAt).count();
                const float fadeDur =
                    std::chrono::duration<float>(kHeaderChipFadeOutDuration).count();
                if (fadeSec < fadeDur) {
                    showMcpHeaderChip = true;
                    mcpChipFade01 = fadeSec / fadeDur;
                } else {
                    showMcpHeaderChip = false;
                    d.mcpHeaderFadeoutActive = false;
                    mcpChipFade01 = 1.0f;
                }
            } else {
                showMcpHeaderChip = false;
            }
        }

        if (showMcpHeaderChip) {
            if (d.cfg.McpAllowRemote) {
                mcpLabelStr = "● MCP LIVE: " + std::to_string(d.cfg.McpPort) + " (LAN)";
            } else {
                mcpLabelStr = "● MCP LIVE: " + std::to_string(d.cfg.McpPort);
            }
        }
    }

    if (!d.cfg.McpEnabled) {
        d.mcpHeaderFadeoutActive = false;
    }

    d.mcpHeaderLastFrameChipShown = showMcpHeaderChip;

    const float mcpW = showMcpHeaderChip ? ImGui::CalcTextSize(mcpLabelStr.c_str()).x : 0.0f;
#else
    const bool showMcpHeaderChip = false;
    const float mcpW = 0.0f;
#endif
    const char* readOnlyLabel = "READ ONLY";
    const bool showReadOnlyChip = d.cfg.ReadOnlyMode;
    const float readOnlyW = showReadOnlyChip ? ImGui::CalcTextSize(readOnlyLabel).x : 0.0f;

    const float btnW = ImGui::CalcTextSize("+ New Issue").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float between = ImGui::GetStyle().ItemSpacing.x * 2.5f;

    float totalRightW = 0.0f;
    bool hasAnyRightElem = false;

    if (showTrackerChip) {
        totalRightW += trW;
        hasAnyRightElem = true;
    }

    if (showReadOnlyChip) {
        if (hasAnyRightElem) {
            totalRightW += between;
        }
        totalRightW += readOnlyW;
        hasAnyRightElem = true;
    }

    if (showMcpHeaderChip) {
        if (hasAnyRightElem) {
            totalRightW += between;
        }
        totalRightW += mcpW;
        hasAnyRightElem = true;
    }

    if (!readOnlyMode) {
        if (hasAnyRightElem) {
            totalRightW += between;
        }
        totalRightW += btnW;
    }

    if (totalRightW > 0.0f) {
        ImGui::SameLine();
        const float targetX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - totalRightW;
        if (targetX > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(targetX);
        }

        if (showTrackerChip) {
            const ImVec4 trDraw = LerpTowardBackground(trColor, headerBgCol, trackerChipFade01);
            ImGui::TextColored(trDraw, "%s", trLabel);
            if (!trTip.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", trTip.c_str());
            }
        }

        if (showReadOnlyChip) {
            if (showTrackerChip) {
                ImGui::SameLine(0.0f, between);
            }
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.22f, 1.0f), "%s", readOnlyLabel);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Read-only mode is enabled in Preferences. Tracker-changing actions are disabled.");
            }
        }
#if defined(SMATCHET_WITH_MCP)
        if ((showTrackerChip || showReadOnlyChip) && showMcpHeaderChip) {
            ImGui::SameLine(0.0f, between);
        }
        if (showMcpHeaderChip) {
            ImVec4 mcpBase = trColor;
            if (!d.cfg.McpEnabled) {
                mcpBase = ImVec4(1.0f, 0.8f, 0.25f, 1.0f);
            } else {
                std::chrono::steady_clock::time_point lastMcpDraw{};
                if (app.TryGetMcpLastClientHttpActivity(&lastMcpDraw) && (nowChip - lastMcpDraw) < kMcpHeaderFlickerRecent) {
                    const int phase = static_cast<int>(ImGui::GetTime() / 0.12) & 1;
                    mcpBase = phase != 0 ? trColor : ImVec4(1.0f, 0.82f, 0.22f, 1.0f);
                }
            }
            const ImVec4 mcpDraw = LerpTowardBackground(mcpBase, headerBgCol, mcpChipFade01);
            ImGui::TextColored(mcpDraw, "%s", mcpLabelStr.c_str());
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
#endif

        if (!readOnlyMode) {
            if (showTrackerChip || showReadOnlyChip || showMcpHeaderChip) {
                ImGui::SameLine(0.0f, between);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.88f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.58f, 0.98f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.38f, 0.78f, 1.0f));
            if (ImGui::Button("+ New Issue")) {
                if (!d.newIssueDraftActive) {
                    const CachedTicket* lastVisibleTicket = nullptr;
                    if (!tickets.empty()) {
                        size_t lastIndex = tickets.size() - 1;
                        if (!d.filteredIndices.empty()) {
                            lastIndex = d.filteredIndices.back();
                        }
                        if (lastIndex < tickets.size()) {
                            lastVisibleTicket = &tickets[lastIndex];
                        }
                    }

                    const std::vector<std::string>& inheritIds = (d.cfg.TrackerType == "Plane") ? d.cfg.NewIssueInheritFieldIdsPlane : d.cfg.NewIssueInheritFieldIds;
                    if (lastVisibleTicket) {
                        d.newIssueDraft = IssueDraftHelpers::FromCachedTicket(
                            *lastVisibleTicket, app.GetAvailableFields(), d.cfg.ProjectKey, d.cfg.DefaultIssueTypeId,
                            d.cfg.DefaultIssueTypeName, inheritIds);
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
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Create a new issue draft and scroll grid to bottom");
            }
        }
    }

    ImGui::Separator();
}
