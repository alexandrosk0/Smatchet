#include "SmatchetLeftViewHistoryUi.h"

#include "CacheBackendKeyPure.h"
#include "ConfigManager.h"
#include "DataFreshnessCue.h"
#include "LeftViewHistoryPure.h"
#include "LeftViewHistoryService.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "Sync/LeftViewReasonPure.h"

#include "imgui.h"

#include "SmatchetLocalizedImGui.h"
// Routes window, button and header literals through the localization wrapper; data-bearing cells use the raw
// (::ImGui) calls, because only exact catalog-English matches rewrite.
#define ImGui SmatchetLocalizedImGui

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kLeftViewColumnCount = 7;

// Per-window state: one window, UI thread only.
struct WindowState {
    char filter[128] = {};
    std::string appliedFilter;
    std::uint64_t appliedRevision = 0;
    bool projectionValid = false;
    std::vector<std::size_t> visible; ///< Row indices matching the filter, newest first.
    std::int64_t selectedId = 0;
    bool clearArmed = false;
    // The details field list, rebuilt when the row or its live answer changes, not every frame.
    std::int64_t detailFieldsRecordId = 0;
    smatchet::LeftViewDetails::Status detailFieldsStatus = smatchet::LeftViewDetails::Status::Idle;
    std::vector<std::pair<std::string, std::string>> detailFields;
};

WindowState& State() {
    static WindowState s_state;
    return s_state;
}

std::string TimeLabel(std::int64_t unixSec) {
    if (unixSec <= 0) {
        return std::string();
    }
    const std::time_t when = static_cast<std::time_t>(unixSec);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &when) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&when, &local) == nullptr) {
        return std::string();
    }
#endif
    char buffer[32];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return std::string(buffer, written);
}

void RefreshProjection(WindowState& state, const smatchet::LeftViewHistoryService& history) {
    const std::string filter(state.filter);
    if (state.projectionValid && state.appliedRevision == history.Revision() && state.appliedFilter == filter) {
        return;
    }
    state.projectionValid = true;
    state.appliedRevision = history.Revision();
    state.appliedFilter = filter;
    state.visible.clear();
    const std::vector<smatchet::LeftViewRecord>& rows = history.Rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::string domain = smatchet::cache_keys::DescribeCacheBackendKey(rows[i].backendKey);
        if (smatchet::LeftViewRowMatches(rows[i], domain, filter)) {
            state.visible.push_back(i);
        }
    }
}

const smatchet::LeftViewRecord* FindSelected(const WindowState& state,
                                             const smatchet::LeftViewHistoryService& history) {
    for (const smatchet::LeftViewRecord& row : history.Rows()) {
        if (row.id == state.selectedId) {
            return &row;
        }
    }
    return nullptr;
}

// A row action chosen this frame; run after the table loop, because it can change the rows being walked.
struct PendingRowAction {
    enum class Kind { None, Details, Browser, Dismiss };
    Kind kind = Kind::None;
    smatchet::LeftViewRecord row;
};

// Every item a row offers, shared by the context menu. Copying acts at once; the rest wait for the loop to end.
void DrawRowMenuItems(const smatchet::LeftViewRecord& row, PendingRowAction& pending) {
    if (ImGui::MenuItem("Open details")) {
        pending.kind = PendingRowAction::Kind::Details;
        pending.row = row;
    }
    if (ImGui::MenuItem("Open in browser", nullptr, false, !row.browseUrl.empty())) {
        pending.kind = PendingRowAction::Kind::Browser;
        pending.row = row;
    }
    if (ImGui::MenuItem("Copy key")) {
        ImGui::SetClipboardText(row.issueKey.c_str());
    }
    if (ImGui::MenuItem("Copy link", nullptr, false, !row.browseUrl.empty())) {
        ImGui::SetClipboardText(row.browseUrl.c_str());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Dismiss")) {
        pending.kind = PendingRowAction::Kind::Dismiss;
        pending.row = row;
    }
}

void DrawTextCell(const std::string& text) { ::ImGui::TextUnformatted(text.c_str()); }

void DrawReasonCell(const smatchet::LeftViewRecord& row) {
    DrawTextCell(row.reason);
    if (!::ImGui::IsItemHovered()) {
        return;
    }
    ::ImGui::BeginTooltip();
    ::ImGui::PushTextWrapPos(::ImGui::GetFontSize() * 30.0f);
    ::ImGui::TextUnformatted(row.reason.c_str());
    if (!row.query.empty()) {
        ::ImGui::Separator();
        ::ImGui::TextUnformatted(row.query.c_str());
    }
    if (row.listedAtUnixSec > 0) {
        // A departure the first reload after a launch found happened some time in this window.
        const std::string span =
            SmatchetLocalization::Format("leftView.between", "Left between %s and %s",
                                         TimeLabel(row.listedAtUnixSec).c_str(), TimeLabel(row.leftAtUnixSec).c_str());
        ::ImGui::TextDisabled("%s", span.c_str());
    }
    ::ImGui::PopTextWrapPos();
    ::ImGui::EndTooltip();
}

void DrawRow(const smatchet::LeftViewRecord& row, WindowState& state, PendingRowAction& pending) {
    ::ImGui::PushID(static_cast<int>(row.id));
    ::ImGui::TableNextRow();
    ::ImGui::TableSetColumnIndex(0);
    const std::string when = TimeLabel(row.leftAtUnixSec);
    const bool selected = state.selectedId == row.id;
    if (::ImGui::Selectable(when.c_str(), selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                ImGuiSelectableFlags_AllowDoubleClick)) {
        state.selectedId = row.id;
        if (::ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            pending.kind = PendingRowAction::Kind::Details;
            pending.row = row;
        }
    }
    if (::ImGui::BeginPopupContextItem("##row_menu")) {
        state.selectedId = row.id;
        DrawRowMenuItems(row, pending);
        ::ImGui::EndPopup();
    }
    ::ImGui::TableSetColumnIndex(1);
    DrawTextCell(row.issueKey);
    ::ImGui::TableSetColumnIndex(2);
    DrawTextCell(row.summary);
    ::ImGui::TableSetColumnIndex(3);
    DrawTextCell(row.viewName);
    ::ImGui::TableSetColumnIndex(4);
    DrawTextCell(smatchet::cache_keys::DescribeCacheBackendKey(row.backendKey));
    ::ImGui::TableSetColumnIndex(5);
    DrawReasonCell(row);
    ::ImGui::TableSetColumnIndex(6);
    DrawTextCell(row.author);
    ::ImGui::PopID();
}

void SetupColumns() {
    const float em = ::ImGui::GetFontSize();
    ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, em * 9.0f);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
    ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, em * 8.0f);
    ImGui::TableSetupColumn("Domain", ImGuiTableColumnFlags_WidthFixed, em * 10.0f);
    ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("By", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
    ::ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
}

// The toolbar, for the row selected in the grid: the same actions as the context menu, reachable by keyboard.
void DrawToolbar(WindowState& state, smatchet::LeftViewHistoryService& history, PendingRowAction& pending) {
    ::ImGui::SetNextItemWidth(::ImGui::GetFontSize() * 14.0f);
    ::ImGui::InputTextWithHint("##left_view_filter", SmatchetLocalization::T("leftView.filter", "Filter"), state.filter,
                               sizeof(state.filter));
    const smatchet::LeftViewRecord* selected = FindSelected(state, history);
    ImGui::SameLine();
    ::ImGui::BeginDisabled(selected == nullptr);
    if (ImGui::Button("Open details") && selected != nullptr) {
        pending.kind = PendingRowAction::Kind::Details;
        pending.row = *selected;
    }
    ImGui::SameLine();
    ::ImGui::BeginDisabled(selected == nullptr || selected->browseUrl.empty());
    if (ImGui::Button("Open in browser") && selected != nullptr) {
        pending.kind = PendingRowAction::Kind::Browser;
        pending.row = *selected;
    }
    ::ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Dismiss") && selected != nullptr) {
        pending.kind = PendingRowAction::Kind::Dismiss;
        pending.row = *selected;
    }
    ::ImGui::EndDisabled();
    ImGui::SameLine();
    // Two-step, as in the Notifications window: a misclick must not destroy the history.
    if (!state.clearArmed) {
        ::ImGui::BeginDisabled(history.Rows().empty());
        if (ImGui::Button("Clear all")) {
            state.clearArmed = true;
        }
        ::ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Confirm clear all")) {
            history.ClearAll();
            state.clearArmed = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) {
            state.clearArmed = false;
        }
    }
}

void DrawGrid(WindowState& state, const smatchet::LeftViewHistoryService& history, float height,
              PendingRowAction& pending) {
    if (state.visible.empty()) {
        if (history.Rows().empty()) {
            ImGui::TextDisabled("No issue has left a view yet.");
        } else {
            ImGui::TextDisabled("No row matches the filter.");
        }
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (!::ImGui::BeginTable("##left_view_grid", kLeftViewColumnCount, flags, ImVec2(0.0f, height))) {
        return;
    }
    SetupColumns();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.visible.size()));
    const std::vector<smatchet::LeftViewRecord>& rows = history.Rows();
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            DrawRow(rows[state.visible[static_cast<std::size_t>(i)]], state, pending);
        }
    }
    ::ImGui::EndTable();
}

void DrawDetailsStatus(const smatchet::LeftViewRecord& row, const smatchet::LeftViewDetails& details,
                       smatchet::LeftViewHistoryService& history, const TrackerConfig& cfg) {
    using Status = smatchet::LeftViewDetails::Status;
    using smatchet::offline::DataFreshness;
    switch (details.status) {
    case Status::Live:
        DataFreshnessCue::DrawInlineBadge(DataFreshness::Fresh);
        break;
    case Status::Loading:
        DataFreshnessCue::DrawInlineBadge(DataFreshness::Refreshing);
        break;
    case Status::Offline:
        DataFreshnessCue::DrawInlineBadge(DataFreshness::CachedOffline);
        break;
    case Status::NotConnected:
        ::ImGui::TextDisabled("%s",
                              SmatchetLocalization::T("leftView.not_connected",
                                                      "Saved values: no open pane is connected to this tracker."));
        break;
    case Status::Failed:
        DataFreshnessCue::DrawInlineBadge(DataFreshness::CachedStale, details.error.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Retry")) {
            history.RequestDetails(row, cfg);
        }
        break;
    case Status::Idle:
    default:
        ::ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("leftView.deleted_saved", "Saved values: deleted on the tracker."));
        break;
    }
}

void DrawDetails(const smatchet::LeftViewRecord& row, WindowState& state, smatchet::LeftViewHistoryService& history,
                 const TrackerConfig& cfg) {
    const smatchet::LeftViewDetails& details = history.Details();
    if (state.detailFieldsRecordId != row.id || state.detailFieldsStatus != details.status) {
        state.detailFieldsRecordId = row.id;
        state.detailFieldsStatus = details.status;
        state.detailFields = smatchet::MergeDetailFields(smatchet::LastKnownFieldsFromJson(row.lastKnownJson),
                                                         details.liveFields, history.FieldLabelsFor(row.backendKey));
    }
    ::ImGui::Separator();
    ::ImGui::TextUnformatted(row.issueKey.c_str());
    ImGui::SameLine();
    DrawDetailsStatus(row, details, history, cfg);
    ImGui::SameLine();
    bool close = false;
    if (ImGui::Button("Close details")) {
        close = true;
    }
    if (::ImGui::BeginTable("##left_view_details", 2,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        for (const std::pair<std::string, std::string>& field : state.detailFields) {
            ::ImGui::TableNextRow();
            ::ImGui::TableSetColumnIndex(0);
            ::ImGui::TextUnformatted(field.first.c_str());
            ::ImGui::TableSetColumnIndex(1);
            ::ImGui::TextWrapped("%s", field.second.c_str());
        }
        ::ImGui::EndTable();
    }
    if (close) {
        history.CloseDetails();
    }
}

void RunPendingAction(const PendingRowAction& pending, WindowState& state, smatchet::LeftViewHistoryService& history,
                      const TrackerConfig& cfg) {
    switch (pending.kind) {
    case PendingRowAction::Kind::Details:
        state.selectedId = pending.row.id;
        history.RequestDetails(pending.row, cfg);
        break;
    case PendingRowAction::Kind::Browser:
        history.OpenUrl(pending.row.browseUrl);
        break;
    case PendingRowAction::Kind::Dismiss:
        history.Dismiss(pending.row.id);
        if (state.selectedId == pending.row.id) {
            state.selectedId = 0;
        }
        break;
    case PendingRowAction::Kind::None:
    default:
        break;
    }
}

} // namespace

// The window title carries a ### id, so the dock slot and the saved settings survive a language change.
void SmatchetDrawLeftViewHistoryWindow(UiDrawSession& d, smatchet::LeftViewHistoryService& history) {
    if (d.requestLeftViewHistoryFocus) {
        ::ImGui::SetNextWindowFocus();
    }
    ::ImGui::SetNextWindowSize(ImVec2(::ImGui::GetFontSize() * 60.0f, ::ImGui::GetFontSize() * 26.0f),
                               ImGuiCond_FirstUseEver);
    if (!SmatchetWindowExpand::BeginWindowWithToggle(d, "Issues That Left Views###LeftViewHistory",
                                                     &d.showLeftViewHistory, d.requestLeftViewHistoryFocus)) {
        return;
    }
    WindowState& state = State();
    PendingRowAction pending;
    DrawToolbar(state, history, pending);
    RefreshProjection(state, history);
    ::ImGui::TextDisabled("%s", SmatchetLocalization::Format("leftView.count", "%d of %d",
                                                             static_cast<int>(state.visible.size()),
                                                             static_cast<int>(history.Rows().size())));

    const smatchet::LeftViewDetails& details = history.Details();
    const smatchet::LeftViewRecord* detailRow = nullptr;
    for (const smatchet::LeftViewRecord& row : history.Rows()) {
        if (details.recordId != 0 && row.id == details.recordId) {
            detailRow = &row;
            break;
        }
    }
    const float available = ::ImGui::GetContentRegionAvail().y;
    const float gridHeight = detailRow != nullptr ? available * 0.55f : 0.0f; // 0 fills the window
    DrawGrid(state, history, gridHeight, pending);
    if (detailRow != nullptr) {
        DrawDetails(*detailRow, state, history, d.cfg);
    }
    ImGui::End();
    RunPendingAction(pending, state, history, d.cfg);
}
