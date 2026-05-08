#include "SmatchetViewsDashboardUi_detail.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetAutocompleteUi.h"
#include "SmatchetUiSession.h"
#include "Views.h"

#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace SmatchetViewsDashboardUiDetail {
void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory) {
    ConfigManager::Save(d.cfg);
    if (pushHistory)
        d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
    app.SyncWithBackend(&d.cfg, &store);
}

void ApplyViewsActiveJqlFromBuffers(AppController& app, UiDrawSession& d, Views& viewState,
                                    const ViewDefinition& activeView) {
    ViewDefinition updated = activeView;
    updated.Name = d.viewNameBuf;
    updated.Jql = d.viewJqlBuf;
    updated.Fields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
    updated.ColumnOrder = d.editingColumnOrder;
    if (viewState.UpdateActive(updated)) {
        d.cfg.JqlQuery = updated.Jql;
        d.cfg.SelectedFields = updated.Fields;
        SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, viewState.GetStore(), true);
    }
}

void DrawJqlQueryEditor(AppController& app, UiDrawSession& d, Views& viewState, const ViewDefinition& activeView) {
    const std::string currentJql(d.viewJqlBuf);
    const bool disableOpenJql = currentJql.empty() || d.cfg.Domain.empty() || d.cfg.TrackerType == "Plane";
    if (disableOpenJql) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(d.cfg.TrackerType == "Plane" ? "Query" : "JQL")) {
        if (d.cfg.TrackerType != "Plane") {
            app.OpenUrl(app.BuildJqlSearchUrl(d.cfg, currentJql));
        }
    }
    if (disableOpenJql) {
        ImGui::EndDisabled();
    }
    if (d.cfg.TrackerType == "Plane") {
        ImGui::SetItemTooltip("Plane does not support opening queries in browser.");
    } else {
        ImGui::SetItemTooltip("Open this query in tracker.");
    }
    ImGui::SameLine();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(d.cfg.TrackerType == "Plane" ? "Plane filter" : "JQL");
    ImGui::SameLine();

    QuerySuggestBuild jqlSuggestBuild;
    QuerySuggestMeta jqlMeta;
    TrackerQueryAcpCallbackUserData jqlCb{};
    jqlCb.session = &d;
    jqlCb.app = &app;
    jqlCb.suggestBuild = &jqlSuggestBuild;
    jqlCb.meta = &jqlMeta;
    jqlCb.kind =
        d.cfg.TrackerType == "Plane" ? TrackerQuerySuggestKind::PlaneFilter : TrackerQuerySuggestKind::JiraJql;

    if (d.jqlAcpWantsJqlInputFocus) {
        ImGui::SetKeyboardFocusHere(0);
        d.jqlAcpWantsJqlInputFocus = false;
    }
    bool jqlInputHot = false;
    ImVec2 jqlFieldRectMin{};
    ImVec2 jqlFieldRectSize{};
    {
        const float applyBtnW =
            ImGui::CalcTextSize(d.cfg.TrackerType == "Plane" ? "Apply Query" : "Apply JQL").x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        const float clearBtnW = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float inputW = ImGui::GetContentRegionAvail().x - applyBtnW - clearBtnW - spacing * 2.0f;
        ImGui::SetNextItemWidth((std::max)(80.0f, inputW));
        ImGui::InputText("##JQL", d.viewJqlBuf, sizeof(d.viewJqlBuf),
                         ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackHistory,
                         &TrackerQueryAcp_InputTextCallback, &jqlCb);
        jqlInputHot = ImGui::IsItemActive() || ImGui::IsItemFocused();
        jqlFieldRectMin = ImGui::GetItemRectMin();
        jqlFieldRectSize = ImGui::GetItemRectSize();
        if (!jqlInputHot) {
            d.jqlAcpListDismissed = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("x##ClearQuery")) {
            std::memset(d.viewJqlBuf, 0, sizeof(d.viewJqlBuf));
            d.jqlAcpAsyncUserItems.clear();
            d.jqlAcpAsyncUserError.clear();
            d.jqlAcpUserSearchFireAt = 0.0;
            d.jqlAcpUserSearchQuery.clear();
            ++d.jqlAcpUserSearchRequestId;
            d.jqlAcpListDismissed = false;
            d.jqlAcpCaretSnapFramesRemaining = 0;
        }
        ImGui::SetItemTooltip("Clear query");
        ImGui::SameLine();
        if (ImGui::Button(d.cfg.TrackerType == "Plane" ? "Apply Query" : "Apply JQL")) {
            SmatchetViewsDashboardUiDetail::ApplyViewsActiveJqlFromBuffers(app, d, viewState, activeView);
        }
        ImGui::SetItemTooltip("Save this query on the active view and sync issues.");
    }
    if (d.cfg.TrackerType == "Plane") {
        ImGui::TextDisabled("field:value AND field:value  ·  Up/Down list  ·  Enter/Tab pick  ·  Esc close list");
    } else {
        ImGui::TextDisabled(
            "JQL tokens + catalog  ·  Up/Down list  ·  Enter/Tab pick  ·  Esc close list  ·  live user search when "
            "typing assignee-style values");
    }

    if (d.jqlWantsApplyFromEnter) {
        d.jqlWantsApplyFromEnter = false;
        SmatchetViewsDashboardUiDetail::ApplyViewsActiveJqlFromBuffers(app, d, viewState, activeView);
    }

    TrackerQueryAcp_TickDebouncedUserSearch(app, d, jqlMeta, jqlSuggestBuild);

    std::vector<QuerySuggestion> merged = jqlSuggestBuild.Items;
    if (!d.jqlAcpAsyncUserItems.empty()) {
        std::unordered_set<std::string> seen;
        for (const auto& s : merged) {
            seen.insert(s.Insert);
        }
        std::copy_if(d.jqlAcpAsyncUserItems.begin(), d.jqlAcpAsyncUserItems.end(), std::back_inserter(merged),
                     [&](const auto& a) { return seen.insert(a.Insert).second; });
    }

    const bool waitingUser =
        d.cfg.TrackerType != "Plane" && jqlMeta.UserValueToken && jqlMeta.UserSearchPrefix.size() >= 2 &&
        d.jqlAcpUserSearchFireAt > 0.0 && ImGui::GetTime() < d.jqlAcpUserSearchFireAt;
    const bool jqlAcpOpen =
        jqlInputHot && !d.jqlAcpListDismissed &&
        (!merged.empty() || waitingUser || (!d.jqlAcpAsyncUserError.empty() && jqlMeta.UserValueToken));
    if (jqlAcpOpen) {
        TrackerQueryAcp_DrawPopup(d, jqlFieldRectMin, jqlFieldRectSize, jqlSuggestBuild, merged);
    }
    TrackerQueryAcp_FlushPendingReplace(d);
}
} // namespace SmatchetViewsDashboardUiDetail
