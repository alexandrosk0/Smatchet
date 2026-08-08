// SMATCHET_DEVIATION(rule=duplication; reason=include prologue, not copy-paste; owner=ui; revisit=2026-12-01)
#include "SmatchetUI.h"
#include "BackendAuditTrail.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "StringUtil.h"
#include "Ui/SmatchetAudit_detail.h"
#include "Ui/SmatchetIconButtons.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class AppController;
/** Join/discard in-flight audit reload so shutdown and reopen don't rely on future destructor blocking. */
void DrainAuditReloadFuture(UiDrawSession& d) {
    if (!d.auditReloadFuture.valid()) {
        d.auditReloadInFlight = false;
        d.auditReloadPending = false;
        return;
    }
    d.auditReloadFuture.wait();
    try {
        (void)d.auditReloadFuture.get();
    } catch (...) { // catch-all-ok: swallow future exception on audit reload drain
    }
    d.auditReloadInFlight = false;
    d.auditReloadPending = false;
}
namespace {
using SmatchetAudit::detail::ActionMatchesFilter;
using SmatchetAudit::detail::ClampRowsPerPage;
using SmatchetAudit::detail::CollectFilteredAuditIndices;
using SmatchetAudit::detail::ComputeAuditPageCount;
using SmatchetAudit::detail::ContainsNoCasePreLower;
using SmatchetAudit::detail::JsonStringValue;
using SmatchetAudit::detail::ResultMatchesFilter;
// Tabs/newlines break TSV paste into spreadsheets.
static std::string AuditSanitizeTsvCell(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\t' || ch == '\n' || ch == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}
static void AppendAuditGridRowTsv(std::string& out, const nlohmann::json& e, const std::string& dataCell) {
    out += AuditSanitizeTsvCell(JsonStringValue(e, "timestamp_local"));
    out += '\t';
    out += AuditSanitizeTsvCell(JsonStringValue(e, "phase"));
    out += '\t';
    out += AuditSanitizeTsvCell(JsonStringValue(e, "action"));
    const bool success = e.value("success", false);
    out += success ? "OK" : "FAIL";
    out += '\t';
    out += AuditSanitizeTsvCell(JsonStringValue(e, "issue_key"));
    out += '\t';
    out += AuditSanitizeTsvCell(JsonStringValue(e, "source"));
    out += '\t';
    out += AuditSanitizeTsvCell(JsonStringValue(e, "error"));
    out += '\t';
    out += AuditSanitizeTsvCell(dataCell.empty() ? std::string("-") : dataCell);
    out += '\n';
}
constexpr auto kAuditFilePollInterval = std::chrono::milliseconds(750);

/** Audit rows may contain arbitrary issue text; strict UTF-8 dump throws — replace invalid sequences instead. */
static std::string AuditJsonDump(const nlohmann::json& j) {
    try {
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) { // catch-all-ok: dump for display
        return "[serialization failed]";
    }
}

/** Runs on worker thread. */
AuditDisplayCachePayload BuildAuditDisplayCachePayload() {
    AuditDisplayCachePayload p;
    Result<std::vector<nlohmann::json>> readResult = BackendAuditTrail::ReadRecentEvents(1000);
    if (readResult.has_value()) {
        p.Events = std::move(readResult.value());
    } else {
        p.ReadError = std::move(readResult.error());
    }
    const std::size_t n = p.Events.size();
    p.FullJson.reserve(n);
    p.DataJson.reserve(n);
    p.SearchLower.reserve(n);
    for (const auto& e : p.Events) {
        p.FullJson.push_back(AuditJsonDump(e));
        p.DataJson.push_back(e.is_object() && e.contains("data") ? AuditJsonDump(e["data"]) : std::string());
        p.SearchLower.push_back(ToLowerAsciiCopy(p.FullJson.back()));
    }
    return p;
}
void ApplyAuditDisplayPayload(UiDrawSession& d, AuditDisplayCachePayload&& p) {
    d.auditCachedReadError = std::move(p.ReadError);
    d.auditCachedEvents = std::move(p.Events);
    d.auditCachedFullJson = std::move(p.FullJson);
    d.auditCachedDataJson = std::move(p.DataJson);
    d.auditCachedSearchLower = std::move(p.SearchLower);
}
void StartAuditReloadWorker(UiDrawSession& d) {
    d.auditReloadInFlight = true;
    d.auditReloadFuture = std::async(std::launch::async, []() { return BuildAuditDisplayCachePayload(); });
}
void RequestAuditReload(UiDrawSession& d) {
    if (d.auditReloadInFlight) {
        d.auditReloadPending = true;
        return;
    }
    StartAuditReloadWorker(d);
}
/** Applies completed worker payload if ready. Returns true when cache was updated this call. */
bool TryCompleteAuditReload(UiDrawSession& d) {
    if (!d.auditReloadInFlight || !d.auditReloadFuture.valid()) {
        return false;
    }
    if (d.auditReloadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    try {
        AuditDisplayCachePayload p = d.auditReloadFuture.get();
        d.auditReloadInFlight = false;
        ApplyAuditDisplayPayload(d, std::move(p));
    } catch (const std::exception& ex) {
        d.auditReloadInFlight = false;
        d.auditCachedReadError = std::string("Audit reload failed: ") + ex.what();
        d.auditCachedEvents.clear();
        d.auditCachedFullJson.clear();
        d.auditCachedDataJson.clear();
        d.auditCachedSearchLower.clear();
    } catch (...) {
        LOG_WARN("SmatchetAuditUi: audit reload failed: unknown exception");
        d.auditReloadInFlight = false;
        d.auditCachedReadError = "Audit reload failed: unknown exception.";
        d.auditCachedEvents.clear();
        d.auditCachedFullJson.clear();
        d.auditCachedDataJson.clear();
        d.auditCachedSearchLower.clear();
    }
    if (d.auditReloadPending) {
        d.auditReloadPending = false;
        StartAuditReloadWorker(d);
    }
    return true;
}

// Render the 8-column audit grid for the current page. Keeps the Begin/EndTable and
// BeginPopup/EndPopup pairs together inside this helper so the orchestrator body stays
// layout-only. Behaviour-identical to the original inline table block.
void DrawAuditTable(UiDrawSession& d, const std::vector<nlohmann::json>& events,
                    const std::vector<std::string>& eventFullJson, const std::vector<std::string>& eventDataJson,
                    const std::vector<std::size_t>& filteredIndices) {
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (!ImGui::BeginTable("BackendAuditTable", 8, flags, ImVec2(0.0f, 360.0f))) {
        return;
    }
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Issue", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    // Empty / zero-results state (P2-L1): bare headers over nothing left "no events" and
    // "filters matched nothing" indistinguishable.
    if (filteredIndices.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s",
                            events.empty() ? "No audit events recorded yet." : "No events match the current filters.");
    }
    const std::size_t begin = static_cast<std::size_t>(d.auditPage) * static_cast<std::size_t>(d.auditRowsPerPage);
    const std::size_t end = (std::min)(filteredIndices.size(), begin + static_cast<std::size_t>(d.auditRowsPerPage));
    for (std::size_t rowIndex = begin; rowIndex < end; ++rowIndex) {
        const std::size_t eventIndex = filteredIndices[rowIndex];
        const nlohmann::json& e = events[eventIndex];
        const std::string action = JsonStringValue(e, "action");
        const bool success = e.value("success", false);
        const std::string& serialized = eventFullJson[eventIndex];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(JsonStringValue(e, "timestamp_local").c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(JsonStringValue(e, "phase").c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(action.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(success ? "OK" : "FAIL");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(JsonStringValue(e, "issue_key").c_str());
        ImGui::TableSetColumnIndex(5);
        ImGui::TextUnformatted(JsonStringValue(e, "source").c_str());
        ImGui::TableSetColumnIndex(6);
        ImGui::TextUnformatted(JsonStringValue(e, "error").c_str());
        ImGui::TableSetColumnIndex(7);
        const std::string& detail = eventDataJson[eventIndex];
        ImGui::TextUnformatted(detail.empty() ? "-" : detail.c_str());
        const std::string popupId = "##audit_row_context_" + std::to_string(eventIndex);
        if (ImGui::BeginPopupContextItem(popupId.c_str())) {
            if (ImGui::MenuItem("Copy audit row JSON")) {
                ImGui::SetClipboardText(serialized.c_str());
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndTable();
}

// Filter toolbar: search box, Action/Result combos, newest-first toggle, rows-per-page input,
// and the read-error banner. Mutating a filter resets to page 0, matching the original inline
// behaviour exactly.
void DrawAuditFilters(UiDrawSession& d, const std::string& readError) {
    ImGui::InputTextWithHint("##audit_search", "Search action / issue / JSON", d.auditSearchBuf,
                             sizeof(d.auditSearchBuf));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    // Item arrays (not "\0"-packed literals) so each entry routes through the catalog — the
    // wrapper Combo translates only the label.
    const char* actionItems[] = {SmatchetLocalization::T("audit.all", "All"),
                                 SmatchetLocalization::T("audit.creates", "Creates"),
                                 SmatchetLocalization::T("audit.updates_transitions", "Updates/Transitions"),
                                 SmatchetLocalization::T("comments.title", "Comments"),
                                 SmatchetLocalization::T("audit.attachments", "Attachments"),
                                 SmatchetLocalization::T("audit.offline", "Offline")};
    ImGui::Combo("Action", &d.auditActionFilter, actionItems, IM_ARRAYSIZE(actionItems));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    const char* resultItems[] = {SmatchetLocalization::T("audit.all", "All"),
                                 SmatchetLocalization::T("audit.success", "Success"),
                                 SmatchetLocalization::T("audit.failure", "Failure")};
    ImGui::Combo("Result", &d.auditResultFilter, resultItems, IM_ARRAYSIZE(resultItems));
    ImGui::SameLine();
    if (ImGui::Checkbox("Newest first", &d.auditNewestFirst)) {
        d.auditPage = 0;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::InputInt("Rows/page", &d.auditRowsPerPage)) {
        d.auditRowsPerPage = ClampRowsPerPage(d.auditRowsPerPage);
        d.auditPage = 0;
    }
    if (!readError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", readError.c_str());
    }
}
} // namespace
void SmatchetUI::drawAuditWindow(AppController& app, UiDrawSession& d) {
    (void)app;
    if (!d.showAuditTrail) {
        DrainAuditReloadFuture(d);
        d.auditTrailWindowWasOpen = false;
        return;
    }
    const bool wantFocus = d.requestAuditTrailFocus;
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
        d.requestAuditTrailFocus = false;
    }
    prepareTopLevelWindow(d, "audit", 920.0f, 520.0f, wantFocus);
    if (!d.auditTrailWindowWasOpen) {
        d.auditTrailWindowWasOpen = true;
        d.auditLastFilePoll = {};
    }
    const auto nowPoll = std::chrono::steady_clock::now();
    const bool pollDue = (d.auditLastFilePoll.time_since_epoch().count() == 0) ||
                         (nowPoll - d.auditLastFilePoll >= kAuditFilePollInterval);
    (void)TryCompleteAuditReload(d);
    SmatchetWindowExpand::BeginWindow(d, "Backend Audit");
    if (!ImGui::Begin("Backend Audit", &d.showAuditTrail)) {
        if (pollDue) {
            d.auditLastFilePoll = nowPoll;
            RequestAuditReload(d);
        }
        ImGui::End();
        return;
    }
    SmatchetWindowExpand::DrawToggle(d);
    repairTopLevelWindow(d, "audit", 420.0f, 300.0f);
    ImGui::Text("Audit file: %s", BackendAuditTrail::GetAuditFilePath().c_str());
    ImGui::SameLine();
    const bool userRefresh = SmatchetIconButton(ICON_FA_ARROWS_ROTATE, "Refresh", "Reload the audit trail.");
    if (userRefresh || pollDue) {
        d.auditLastFilePoll = nowPoll;
        RequestAuditReload(d);
    }
    if (d.auditReloadInFlight) {
        ImGui::SameLine();
        ImGui::TextDisabled("(loading…)");
    }
    const std::vector<nlohmann::json>& events = d.auditCachedEvents;
    const std::vector<std::string>& eventFullJson = d.auditCachedFullJson;
    const std::vector<std::string>& eventDataJson = d.auditCachedDataJson;
    const std::string& readError = d.auditCachedReadError;
    DrawAuditFilters(d, readError);
    const std::string query = d.auditSearchBuf;
    const std::string queryLower = ToLowerAsciiCopy(query);
    const std::vector<std::size_t> filteredIndices = CollectFilteredAuditIndices(
        events, d.auditCachedSearchLower, d.auditActionFilter, d.auditResultFilter, queryLower, d.auditNewestFirst);
    d.auditRowsPerPage = ClampRowsPerPage(d.auditRowsPerPage);
    const int pageCount = ComputeAuditPageCount(filteredIndices.size(), d.auditRowsPerPage);
    if (d.auditPage < 0)
        d.auditPage = 0;
    if (d.auditPage >= pageCount)
        d.auditPage = pageCount - 1;
    if (ImGui::Button("Prev##auditPage") && d.auditPage > 0) {
        --d.auditPage;
    }
    ImGui::SameLine();
    ImGui::Text("Page %d/%d (%zu rows)", d.auditPage + 1, pageCount, filteredIndices.size());
    ImGui::SameLine();
    if (ImGui::Button("Next##auditPage") && d.auditPage + 1 < pageCount) {
        ++d.auditPage;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy filtered table (TSV)")) {
        std::string clip;
        clip.reserve(filteredIndices.size() * 192u + 128u);
        clip += "Time\tPhase\tAction\tResult\tIssue\tSource\tError\tDetails\n";
        for (const std::size_t eventIndex : filteredIndices) {
            AppendAuditGridRowTsv(clip, events[eventIndex], eventDataJson[eventIndex]);
        }
        ImGui::SetClipboardText(clip.c_str());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(
            "Copy all rows matching current filters and search (not only this page), as tab-separated text.");
    }
    DrawAuditTable(d, events, eventFullJson, eventDataJson, filteredIndices);
    ImGui::TextDisabled("Copy filtered table: all matching rows as TSV. Right-click a row detail for full JSON. Cached "
                        "tail shows latest "
                        "1000 events.");
    ImGui::End();
}
