#include "SmatchetViewsDashboardUi_detail.h"
#include "SmatchetJqlProjectPill_detail.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "ITrackerBackend.h"
#include "JqlProjectScope.h"
#include "PlaneProjectScope.h"
#include "FieldCatalogCache.h"
#include "SmatchetAutocompleteUi.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "Views.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace SmatchetViewsDashboardUiDetail {

void SnapshotActiveViewIfNeeded(UiDrawSession& d, const ViewDefinition& view) {
    if (d.viewsHasOriginalSnapshot) {
        return;
    }
    d.viewsOriginalSnapshot = view;
    d.viewsHasOriginalSnapshot = true;
}

void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory) {
    ConfigManager::Save(d.cfg);
    if (pushHistory) {
        d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
    }
    app.SyncWithBackend(&d.cfg, &store);
}

void ApplyViewsActiveJqlFromBuffers(AppController& app, UiDrawSession& d, Views& viewState,
                                    const ViewDefinition& activeView) {
    ViewDefinition updated = activeView;
    updated.Name = d.viewNameBuf;
    updated.Jql = d.viewJqlEditor.buf;
    // Authoritative selection set, not the truncating buffer (#views-field-uncheck).
    updated.Fields = SmatchetViewsDashboardUiDetail::ToSortedVector(d.selectedFieldSet);
    updated.ColumnOrder = d.editingColumnOrder;
    if (viewState.UpdateActive(updated)) {
        d.cfg.JqlQuery = updated.Jql;
        d.cfg.SelectedFields = updated.Fields;
        SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, viewState.GetStore(), true);
    }
}

void DrawVerticalSplitter(const char* id, UiDrawSession& d, float* height, float minHeight, float maxHeight) {
    if (!height) {
        return;
    }
    ImGui::InvisibleButton(id, ImVec2(-FLT_MIN, 6.0f));
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
        *height += ImGui::GetIO().MouseDelta.y;
        if (*height < minHeight) {
            *height = minHeight;
        }
        if (*height > maxHeight) {
            *height = maxHeight;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ConfigManager::Save(d.cfg);
    }
    ImVec2 pMin = ImGui::GetItemRectMin();
    ImVec2 pMax = ImGui::GetItemRectMax();
    pMin.y += 2.0f;
    pMax.y -= 2.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(pMin, pMax, ImGui::GetColorU32(ImGuiCol_Separator));
}

void DrawHorizontalSplitter(const char* id, UiDrawSession& d, float* widthLeft, float minLeft, float maxLeft) {
    if (!widthLeft) {
        return;
    }
    ImGui::InvisibleButton(id, ImVec2(6.0f, -FLT_MIN));
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive()) {
        *widthLeft += ImGui::GetIO().MouseDelta.x;
        if (*widthLeft < minLeft) {
            *widthLeft = minLeft;
        }
        if (*widthLeft > maxLeft) {
            *widthLeft = maxLeft;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ConfigManager::Save(d.cfg);
    }
    ImVec2 pMin = ImGui::GetItemRectMin();
    ImVec2 pMax = ImGui::GetItemRectMax();
    pMin.x += 2.0f;
    pMax.x -= 2.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(pMin, pMax, ImGui::GetColorU32(ImGuiCol_Separator));
}

void DrawDragHandle(const char* id, int rowIndex, const char* payloadType) {
    // Render the handle as a small invisible button so it has its own hit rect
    // (separate from the row body). The visual is six dots drawn manually.
    ImGui::PushID(id);
    const float handleWidth = ImGui::GetFontSize() * 0.75f;
    const float handleHeight = ImGui::GetFrameHeight();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##handle", ImVec2(handleWidth, handleHeight));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(payloadType, &rowIndex, sizeof(int));
        ImGui::TextDisabled("Move row %d", rowIndex);
        ImGui::EndDragDropSource();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float cx = cursor.x + handleWidth * 0.5f;
    const float cy0 = cursor.y + handleHeight * 0.25f;
    const float cy1 = cursor.y + handleHeight * 0.50f;
    const float cy2 = cursor.y + handleHeight * 0.75f;
    const float r = 1.5f;
    const float dx = 2.5f;
    dl->AddCircleFilled(ImVec2(cx - dx, cy0), r, col);
    dl->AddCircleFilled(ImVec2(cx + dx, cy0), r, col);
    dl->AddCircleFilled(ImVec2(cx - dx, cy1), r, col);
    dl->AddCircleFilled(ImVec2(cx + dx, cy1), r, col);
    dl->AddCircleFilled(ImVec2(cx - dx, cy2), r, col);
    dl->AddCircleFilled(ImVec2(cx + dx, cy2), r, col);
    ImGui::PopID();
    ImGui::SameLine();
}

bool HandleRowReorder(int rowIndex, std::vector<std::string>& order, int* keyboardFocusRow, const char* payloadType) {
    bool mutated = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(payloadType)) {
            if (payload->DataSize == static_cast<int>(sizeof(int))) {
                int src = *static_cast<const int*>(payload->Data);
                if (src >= 0 && src < static_cast<int>(order.size()) && rowIndex >= 0 &&
                    rowIndex < static_cast<int>(order.size()) && src != rowIndex) {
                    std::string moved = order[static_cast<size_t>(src)];
                    order.erase(order.begin() + src);
                    int dst = rowIndex;
                    if (src < dst) {
                        --dst; // adjust for erase
                    }
                    order.insert(order.begin() + dst, std::move(moved));
                    if (keyboardFocusRow) {
                        *keyboardFocusRow = dst;
                    }
                    mutated = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    // Keyboard reorder: Alt+Up / Alt+Down on the row whose index matches
    // `keyboardFocusRow`. Gate on window-level focus (not IsItemFocused) because the
    // focused item-ID belongs to the PREVIOUS Selectable widget — after a swap the
    // PushID + label combo at this index is different, so the focus check would fail
    // on every press after the first. Window focus is the correct shortcut scope.
    // Hot-path: this helper runs once per visible row, every frame. Bail out on the
    // cheapest checks first (focus-row mismatch, then `KeyAlt` test against the cached
    // IO struct) before paying for `IsWindowFocused`. On a typical "no Alt held" frame
    // with N rows, we now exit at the `io.KeyAlt` branch and skip 3N+ ImGui calls.
    if (!keyboardFocusRow || *keyboardFocusRow != rowIndex) {
        return mutated;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyAlt) {
        return mutated;
    }
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        return mutated;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && rowIndex > 0) {
        std::swap(order[static_cast<size_t>(rowIndex)], order[static_cast<size_t>(rowIndex - 1)]);
        *keyboardFocusRow = rowIndex - 1;
        mutated = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && rowIndex + 1 < static_cast<int>(order.size())) {
        std::swap(order[static_cast<size_t>(rowIndex)], order[static_cast<size_t>(rowIndex + 1)]);
        *keyboardFocusRow = rowIndex + 1;
        mutated = true;
    }
    return mutated;
}

void TickDragDropAutoScroll() {
    // Nudge ScrollY when a drag-drop payload is in flight and the mouse is hovering
    // near the top or bottom edge of the current (scrollable) window. Public-API only
    // so it works inside any BeginChild block. Call once per frame near the end of
    // the scrollable region (before EndChild).
    // Stable-API check: GetDragDropPayload returns non-null whenever a payload is in
    // flight (it does NOT require being inside a BeginDragDropTarget block, unlike
    // AcceptDragDropPayload). IsDragDropActive() is imgui_internal-only.
    if (ImGui::GetDragDropPayload() == nullptr) {
        return;
    }
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < windowPos.x || mouse.x > windowPos.x + windowSize.x || mouse.y < windowPos.y ||
        mouse.y > windowPos.y + windowSize.y) {
        return;
    }
    constexpr float kEdgeBand = 32.0f;
    constexpr float kMaxSpeed = 18.0f;
    const float topEdge = windowPos.y;
    const float bottomEdge = windowPos.y + windowSize.y;
    float delta = 0.0f;
    if (mouse.y < topEdge + kEdgeBand) {
        const float t = (topEdge + kEdgeBand - mouse.y) / kEdgeBand;
        delta = -kMaxSpeed * t;
    } else if (mouse.y > bottomEdge - kEdgeBand) {
        const float t = (mouse.y - (bottomEdge - kEdgeBand)) / kEdgeBand;
        delta = kMaxSpeed * t;
    }
    if (delta != 0.0f) {
        const float maxY = ImGui::GetScrollMaxY();
        const float newY = (std::max)(0.0f, (std::min)(maxY, ImGui::GetScrollY() + delta));
        ImGui::SetScrollY(newY);
    }
}

namespace {
// Project pill: clickable label under the query bar whose popup clamps the active view to
// a single project. Jira uses JQL `project = …`; Plane stores `project_id` in structured JSON.
void DrawJqlProjectPill(AppController& app, UiDrawSession& d) {
    const std::string currentJql(d.viewJqlEditor.buf);
    const ITrackerBackend* backend = app.GetTrackerBackend();
    const bool isPlane = smatchet::tracker::IsPlaneBackendType(d.cfg.TrackerType);
    const std::string backendKind = isPlane ? std::string("Plane") : std::string("Jira");
    const std::string endpoint =
        isPlane ? (d.cfg.PlaneUrl + std::string("|") + d.cfg.PlaneWorkspaceSlug) : d.cfg.Domain;
    const std::string scopeProj = backend ? backend->Connectivity().ExtractProjectFromQuery(currentJql) : std::string();
    const bool single = !scopeProj.empty();
    const char* pillLabel = nullptr;
    std::string pillBuf;
    if (single) {
        // The loc fmt is "Project: %s"; one snprintf per frame is fine — this is header chrome,
        // not per-row hot path.
        const char* fmt = SmatchetLocalization::T("view.projectPill.single", "Project: %s");
        char tmp[128];
        std::snprintf(tmp, sizeof(tmp), fmt, scopeProj.c_str());
        pillBuf = tmp;
        pillLabel = pillBuf.c_str();
    } else {
        pillLabel = SmatchetLocalization::T("view.projectPill.multi", "Project: multi");
    }
    ImGui::PushStyleColor(ImGuiCol_Button,
                          single ? ImVec4(0.18f, 0.40f, 0.66f, 0.65f) : ImVec4(0.50f, 0.42f, 0.18f, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          single ? ImVec4(0.24f, 0.50f, 0.78f, 0.85f) : ImVec4(0.62f, 0.52f, 0.22f, 0.85f));
    if (ImGui::SmallButton(pillLabel)) {
        ImGui::OpenPopup("##ProjectPillPopup");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", SmatchetLocalization::T(
                                    single ? "view.projectPill.tooltip.single" : "view.projectPill.tooltip.multi",
                                    single ? "Active view is scoped to a single project. Click to switch project."
                                           : "Active view spans multiple projects. Click to pick a single project."));
    }
    // Pillar 2 (#767): snapshot the cached-project list on the popup open-edge instead of
    // re-reading disk every frame the popup is open. The popup's live open-state drives the
    // helper, which refreshes the shared UiDrawSession snapshot only on the closed→open edge.
    RefreshCachedProjectsSnapshotOnOpen(d, ImGui::IsPopupOpen("##ProjectPillPopup"), d.projectPillPopupWasOpen);
    if (ImGui::BeginPopup("##ProjectPillPopup")) {
        const std::vector<FieldCatalogCache::CachedProjectEntry>& cached = d.cachedProjectsSnapshot;
        int shown = 0;
        for (const auto& e : cached) {
            if (!SmatchetJqlProjectPill::detail::EntryPassesPillFilter(e, backendKind, endpoint)) {
                continue;
            }
            ImGui::PushID(shown);
            if (ImGui::Selectable(e.projectKey.c_str(), e.projectKey == scopeProj)) {
                const std::string newJql = isPlane ? PlaneProjectScope::SetProjectInQuery(currentJql, e.projectKey)
                                                   : JqlProjectScope::SetProjectClause(currentJql, e.projectKey);
                CopyStringToBuffer(d.viewJqlEditor.buf, newJql);
                d.viewsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
            ++shown;
        }
        if (shown == 0) {
            ImGui::TextDisabled("%s", SmatchetLocalization::T("view.projectPill.empty", "(no cached projects)"));
        }
        ImGui::EndPopup();
    }
}
} // namespace

void DrawJqlQueryEditorEmbedded(AppController& app, UiDrawSession& d, JqlEditorState& st, bool drawProjectPill,
                                const char* hint) {
    // Lightweight variant of DrawJqlQueryEditor — used when the surrounding tab
    // already provides the label / open-in-browser chrome. Reuses the same
    // autocomplete machinery (TrackerQueryAcp_*) and the JqlEditorState buffer so the
    // legacy flow and modern flow stay in lock-step.
    QuerySuggestBuild jqlSuggestBuild;
    QuerySuggestMeta jqlMeta;
    TrackerQueryAcpCallbackUserData jqlCb{};
    jqlCb.session = &d;
    jqlCb.editor = &st;
    jqlCb.fields = &app.GetAvailableFields();
    jqlCb.users = &app.GetAvailableUsers();
    jqlCb.suggestBuild = &jqlSuggestBuild;
    jqlCb.meta = &jqlMeta;
    jqlCb.kind = smatchet::tracker::IsPlaneBackendType(d.cfg.TrackerType) ? TrackerQuerySuggestKind::PlaneFilter
                                                                          : TrackerQuerySuggestKind::JiraJql;

    if (st.jqlAcpWantsJqlInputFocus) {
        ImGui::SetKeyboardFocusHere(0);
        st.jqlAcpWantsJqlInputFocus = false;
    }

    const float clearBtnW = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float inputW = ImGui::GetContentRegionAvail().x - clearBtnW - spacing;
    ImGui::SetNextItemWidth((std::max)(120.0f, inputW));
    // Optional placeholder (omnibar passes one) so each of the app's search entry points
    // states what it searches; the dashboard editor keeps its surrounding label chrome.
    if (hint != nullptr) {
        ImGui::InputTextWithHint("##JQLEmbedded", hint, st.buf, sizeof(st.buf),
                                 ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackHistory,
                                 &TrackerQueryAcp_InputTextCallback, &jqlCb);
    } else {
        ImGui::InputText("##JQLEmbedded", st.buf, sizeof(st.buf),
                         ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackHistory,
                         &TrackerQueryAcp_InputTextCallback, &jqlCb);
    }
    const bool jqlInputHot = ImGui::IsItemActive() || ImGui::IsItemFocused();
    const ImVec2 jqlFieldRectMin = ImGui::GetItemRectMin();
    const ImVec2 jqlFieldRectSize = ImGui::GetItemRectSize();
    if (!jqlInputHot) {
        st.jqlAcpListDismissed = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("x##ClearQueryEmbedded")) {
        std::memset(st.buf, 0, sizeof(st.buf));
        st.jqlAcpAsyncUserItems.clear();
        st.jqlAcpAsyncUserError.clear();
        st.jqlAcpUserSearchFireAt = 0.0;
        st.jqlAcpUserSearchQuery.clear();
        ++st.jqlAcpUserSearchRequestId;
        st.jqlAcpListDismissed = false;
        st.jqlAcpCaretSnapFramesRemaining = 0;
        // Only the dashboard/views editor tracks saved-view dirtiness; the omnibar
        // (drawProjectPill=false) clears a transient search and must not flag the saved view (#4).
        if (drawProjectPill) {
            d.viewsDirty = true;
        }
    }
    ImGui::SetItemTooltip("Clear query");

    TrackerQueryAcp_TickDebouncedUserSearch(app, d, st, jqlMeta, jqlSuggestBuild);
    // Name the ids already IN the query (saved view, restored session, pasted clause) —
    // the search above only fires while typing a value token, so it never covers these.
    // Jira-only: it is the one backend with a by-accountId lookup.
    if (smatchet::tracker::BackendIndexFromType(d.cfg.TrackerType) == smatchet::tracker::kBackendJira) {
        TrackerQueryAcp_TickAccountIdResolve(app, app.GetAvailableUsers(), st);
    }

    std::vector<QuerySuggestion> merged = jqlSuggestBuild.Items;
    if (!st.jqlAcpAsyncUserItems.empty()) {
        std::unordered_set<std::string> seen;
        for (const auto& s : merged) {
            seen.insert(s.Insert);
        }
        std::copy_if(st.jqlAcpAsyncUserItems.begin(), st.jqlAcpAsyncUserItems.end(), std::back_inserter(merged),
                     [&](const auto& a) { return seen.insert(a.Insert).second; });
    }

    const bool waitingUser = !smatchet::tracker::IsPlaneBackendType(d.cfg.TrackerType) && jqlMeta.UserValueToken &&
                             jqlMeta.UserSearchPrefix.size() >= 2 && st.jqlAcpUserSearchFireAt > 0.0 &&
                             ImGui::GetTime() < st.jqlAcpUserSearchFireAt;
    const bool jqlAcpOpen =
        jqlInputHot && !st.jqlAcpListDismissed &&
        (!merged.empty() || waitingUser || (!st.jqlAcpAsyncUserError.empty() && jqlMeta.UserValueToken));
    if (jqlAcpOpen) {
        TrackerQueryAcp_DrawPopup(d, st, jqlFieldRectMin, jqlFieldRectSize, jqlSuggestBuild, merged);
    }
    TrackerQueryAcp_FlushPendingReplace(st);

    // Readable echo of the query: user clauses carry an opaque account id (the only form
    // Jira Cloud matches a user on), so spell those ids out as names right under the bar.
    TrackerQueryAcp_DrawUserEcho(app.GetAvailableUsers(), st);

    // Project pill beneath the query bar — pick a single project scope for the active view.
    // Dashboard-only: the pill is hard-bound to d.viewJqlEditor, so the omnibar opts out.
    if (drawProjectPill) {
        DrawJqlProjectPill(app, d);
    }
}

} // namespace SmatchetViewsDashboardUiDetail
