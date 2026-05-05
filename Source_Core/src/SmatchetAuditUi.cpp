#include "SmatchetUI.h"
#include "BackendAuditTrail.h"
#include "SmatchetUiSession.h"
#include "StringUtil.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <string>
#include <vector>
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
    } catch (...) {
    }
    d.auditReloadInFlight = false;
    d.auditReloadPending = false;
}
namespace {
std::string JsonStringValue(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) {
        return std::string();
    }
    const auto& v = j[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<unsigned long long>());
    }
    return v.dump();
}
bool ContainsNoCasePreLower(const std::string& haystackLower, const std::string& needleLower) {
    if (needleLower.empty()) {
        return true;
    }
    return haystackLower.find(needleLower) != std::string::npos;
}
bool ActionMatchesFilter(const std::string& action, int filter) {
    switch (filter) {
    case 1:
        return action.find("create") != std::string::npos && action.find("offline") == std::string::npos;
    case 2:
        return action.find("update") != std::string::npos || action.find("transition") != std::string::npos ||
               action.find("field_edit") != std::string::npos;
    case 3:
        return action.find("comment") != std::string::npos;
    case 4:
        return action.find("attach") != std::string::npos;
    case 5:
        return action.find("offline") != std::string::npos;
    default:
        return true;
    }
}
bool ResultMatchesFilter(bool success, int filter) {
    if (filter == 1)
        return success;
    if (filter == 2)
        return !success;
    return true;
}
int ClampRowsPerPage(int value) {
    if (value < 25)
        return 25;
    if (value > 1000)
        return 1000;
    return value;
}
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
    } catch (...) {
        return "[serialization failed]";
    }
}

/** Runs on worker thread. */
AuditDisplayCachePayload BuildAuditDisplayCachePayload() {
    AuditDisplayCachePayload p;
    std::string err;
    std::vector<nlohmann::json> events = BackendAuditTrail::ReadRecentEvents(1000, &err);
    p.ReadError = std::move(err);
    p.Events = std::move(events);
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
} // namespace
void SmatchetUI::drawAuditWindow(AppController& app, UiDrawSession& d) {
    (void)app;
    if (!d.showAuditTrail) {
        DrainAuditReloadFuture(d);
        d.auditTrailWindowWasOpen = false;
        return;
    }
    if (d.requestAuditTrailFocus) {
        ImGui::SetNextWindowFocus();
        d.requestAuditTrailFocus = false;
    }
    ImGui::SetNextWindowSize(ImVec2(920.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!d.auditTrailWindowWasOpen) {
        d.auditTrailWindowWasOpen = true;
        d.auditLastFilePoll = {};
    }
    const auto nowPoll = std::chrono::steady_clock::now();
    const bool pollDue = (d.auditLastFilePoll.time_since_epoch().count() == 0) ||
                         (nowPoll - d.auditLastFilePoll >= kAuditFilePollInterval);
    (void)TryCompleteAuditReload(d);
    if (!ImGui::Begin("Backend Audit", &d.showAuditTrail)) {
        if (pollDue) {
            d.auditLastFilePoll = nowPoll;
            RequestAuditReload(d);
        }
        ImGui::End();
        return;
    }
    ImGui::Text("Audit file: %s", BackendAuditTrail::GetAuditFilePath().c_str());
    ImGui::SameLine();
    const bool userRefresh = ImGui::Button("Refresh");
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
    ImGui::InputTextWithHint("##audit_search", "Search action / issue / JSON", d.auditSearchBuf,
                             sizeof(d.auditSearchBuf));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::Combo("Action", &d.auditActionFilter,
                 "All\0Creates\0Updates/Transitions\0Comments\0Attachments\0Offline\0\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("Result", &d.auditResultFilter, "All\0Success\0Failure\0\0");
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
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    std::vector<std::size_t> filteredIndices;
    filteredIndices.reserve(events.size());
    const std::string query = d.auditSearchBuf;
    const std::string queryLower = ToLowerAsciiCopy(query);
    const auto collectEvent = [&](std::size_t eventIndex) {
        const nlohmann::json& e = events[eventIndex];
        const std::string action = JsonStringValue(e, "action");
        const bool success = e.value("success", false);
        if (!ActionMatchesFilter(action, d.auditActionFilter) || !ResultMatchesFilter(success, d.auditResultFilter)) {
            return;
        }
        if (eventIndex >= d.auditCachedSearchLower.size()) {
            return;
        }
        if (!ContainsNoCasePreLower(d.auditCachedSearchLower[eventIndex], queryLower)) {
            return;
        }
        filteredIndices.push_back(eventIndex);
    };
    if (d.auditNewestFirst) {
        for (std::size_t i = events.size(); i > 0; --i) {
            collectEvent(i - 1);
        }
    } else {
        for (std::size_t idx = 0; idx < events.size(); ++idx) {
            collectEvent(idx);
        }
    }
    d.auditRowsPerPage = ClampRowsPerPage(d.auditRowsPerPage);
    const int pageCount =
        filteredIndices.empty()
            ? 1
            : static_cast<int>((filteredIndices.size() + static_cast<std::size_t>(d.auditRowsPerPage) - 1) /
                               static_cast<std::size_t>(d.auditRowsPerPage));
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
    if (ImGui::BeginTable("BackendAuditTable", 8, flags, ImVec2(0.0f, 360.0f))) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Issue", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        const std::size_t begin = static_cast<std::size_t>(d.auditPage) * static_cast<std::size_t>(d.auditRowsPerPage);
        const std::size_t end =
            (std::min)(filteredIndices.size(), begin + static_cast<std::size_t>(d.auditRowsPerPage));
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
    ImGui::TextDisabled("Copy filtered table: all matching rows as TSV. Right-click a row detail for full JSON. Cached "
                        "tail shows latest "
                        "1000+ events.");
    ImGui::End();
}






