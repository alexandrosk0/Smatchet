#include "SmatchetUI.h"

#include "AppController.h"
#include "TrackerHttpUtils.h"
#include "IssueDraft.h"
#include "IssueTableSerializer.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"
#include "StringUtil.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

IssueTableSerializer::Format BulkFormatFromIndex(int idx) {
    switch (idx) {
    case 1:
        return IssueTableSerializer::Format::Csv;
    case 2:
        return IssueTableSerializer::Format::Tsv;
    case 3:
        return IssueTableSerializer::Format::Json;
    default:
        return IssueTableSerializer::Format::Auto;
    }
}

int BulkImportTextResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* buf = static_cast<std::vector<char>*>(data->UserData);
        buf->resize(static_cast<size_t>(data->BufTextLen) + 1);
        data->Buf = buf->data();
    }
    return 0;
}

bool ReadEntireFile(const std::string& path, std::string& outText, std::string& outError) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        outError = "Failed to open file: " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    outText = ss.str();
    return true;
}

bool WriteEntireFile(const std::string& path, const std::string& text, std::string& outError) {
    std::ofstream f(path, std::ios::binary);
    if (!f.good()) {
        outError = "Failed to open file for write: " + path;
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

static const CachedTicket* BulkImportFindTicketInSnapshot(const std::string& issueKey,
                                                          const std::vector<CachedTicket>* tickets) {
    if (!tickets || issueKey.empty()) {
        return nullptr;
    }
    auto it = std::find_if(tickets->begin(), tickets->end(), [&](const auto& t) {
        return t.id == issueKey;
    });
    if (it != tickets->end()) {
        return &(*it);
    }
    return nullptr;
}

/** Update row identical to cached ticket (same rules as Changes column "no changes"). */
static bool BulkImportRowIsNoopUpdate(const IssueTableSerializer::ImportRow& row,
                                      const std::vector<CachedTicket>* tickets) {
    if (row.Draft.ExistingIssueKey.empty() || !row.Error.empty() || !tickets) {
        return false;
    }
    const CachedTicket* t = BulkImportFindTicketInSnapshot(row.Draft.ExistingIssueKey, tickets);
    if (!t) {
        return false;
    }
    return IssueDraftHelpers::ComputeFieldChanges(row.Draft, *t).empty();
}

/** True once a row has left the dispatch pipeline (success, failure, skip, or parse error). */
static bool BulkImportStatusIsTerminal(const std::string& status) {
    if (status.empty()) {
        return false;
    }
    if (status == "queued" || status == "waiting for cache…" || status == "submitting...") {
        return false;
    }
    return true;
}

} // anonymous namespace

void SmatchetUI::drawBulkImportWindow(AppController& app, UiDrawSession& d) {
    if (!d.showBulkImport) {
        if (d.bulkImportWasOpen) {
            d.bulkImportTextBuf.clear();
            d.bulkImportPathBuf[0] = '\0';
            d.bulkImportFormatSel = 0;
            d.bulkImportPreview = {};
            d.bulkImportStatus.clear();
            d.bulkImportFutures.clear();
            d.bulkImportCompleted = 0;
            d.bulkImportRunning = false;
            d.bulkImportError.clear();
            d.bulkImportWasOpen = false;
        }
        return;
    }
    d.bulkImportWasOpen = true;

    prepareTopLevelWindow(d, "bulk_import", 900.0f, 600.0f);
    if (!ImGui::Begin("Bulk import tickets", &d.showBulkImport)) {
        ImGui::End();
        return;
    }
    repairTopLevelWindow(d, "bulk_import", 520.0f, 360.0f);

    if (d.bulkImportTextBuf.empty())
        d.bulkImportTextBuf.assign(1, '\0');

    ImGui::TextUnformatted("Source:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(520);
    ImGui::InputText("##bulkImportPath", d.bulkImportPathBuf, sizeof(d.bulkImportPathBuf));
    ImGui::SameLine();
    if (ImGui::Button("Load file")) {
        std::string text, err;
        if (ReadEntireFile(d.bulkImportPathBuf, text, err)) {
            d.bulkImportTextBuf.assign(text.begin(), text.end());
            d.bulkImportTextBuf.push_back('\0');
            d.bulkImportError.clear();
        } else {
            d.bulkImportError = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste clipboard")) {
        const char* clip = ImGui::GetClipboardText();
        if (clip && *clip) {
            const std::string s(clip);
            d.bulkImportTextBuf.assign(s.begin(), s.end());
            d.bulkImportTextBuf.push_back('\0');
            d.bulkImportError.clear();
        } else {
            d.bulkImportError = "Clipboard is empty.";
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* kFormats[] = {"Auto", "CSV", "TSV", "JSON"};
    ImGui::Combo("##bulkImportFmt", &d.bulkImportFormatSel, kFormats, IM_ARRAYSIZE(kFormats));
    ImGui::SameLine();
    if (ImGui::Button("Parse preview")) {
        const std::string text(d.bulkImportTextBuf.data());
        const IssueTableSerializer::Format fmt = BulkFormatFromIndex(d.bulkImportFormatSel);
        d.bulkImportPreview = IssueTableSerializer::ParseDrafts(text, fmt, app.GetAvailableFields(), d.cfg.ProjectKey,
                                                                d.cfg.DefaultIssueTypeId, d.cfg.DefaultIssueTypeName);
        d.bulkImportStatus.assign(d.bulkImportPreview.Rows.size(), std::string());
        d.bulkImportError = d.bulkImportPreview.Error;
        d.bulkImportCompleted = 0;
        d.bulkImportRunning = false;
        d.bulkImportFutures.clear();
        d.bulkImportFutures.resize(d.bulkImportPreview.Rows.size());
    }

    if (!d.bulkImportError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", d.bulkImportError.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Paste / edit source text here (headers = field ids or display names):");
    ImGui::InputTextMultiline("##bulkImportText", d.bulkImportTextBuf.data(), d.bulkImportTextBuf.size(),
                              ImVec2(-FLT_MIN, 160.0f),
                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize,
                              &BulkImportTextResizeCallback, &d.bulkImportTextBuf);

    ImGui::Separator();
    ImGui::Text("Parsed rows: %zu", d.bulkImportPreview.Rows.size());
    ImGui::SameLine();
    const int maxConcurrent = (std::max)(1, d.cfg.ImportMaxConcurrent);
    ImGui::Text("| Max concurrent: %d", maxConcurrent);
    ImGui::SameLine();
    const bool canRun = !d.bulkImportPreview.Rows.empty() && !d.bulkImportRunning;
    if (!canRun)
        ImGui::BeginDisabled();
    if (ImGui::Button("Run import")) {
        d.gridEditError.clear();
        d.gridEditSuccess.clear();
        d.bulkImportRunning = true;
        d.bulkImportCompleted = 0;
        d.bulkImportStatus.assign(d.bulkImportPreview.Rows.size(), "queued");
        d.bulkImportFutures.clear();
        d.bulkImportFutures.resize(d.bulkImportPreview.Rows.size());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
        ImGui::TextUnformatted(
            "Creates new issues and updates existing keys. Rows with no field changes vs the cached "
            "ticket are skipped (no Jira call). Update rows still fetching into cache wait until the "
            "load finishes so skips can apply; keys that never appear in cache are still sent to Jira.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (!canRun)
        ImGui::EndDisabled();

    {
        std::vector<std::string> keysNeedingHydration;
        for (const auto& previewRow : d.bulkImportPreview.Rows) {
            const std::string& ek = previewRow.Draft.ExistingIssueKey;
            if (ek.empty()) {
                continue;
            }
            keysNeedingHydration.push_back(ek);
        }
        app.PrefetchIssueTicketsForKeys(keysNeedingHydration);
    }

    const auto ticketsSnap = app.GetActiveTicketsSnapshot();

    // Pump submissions + completions each frame.
    if (d.bulkImportRunning) {
        // Submit up to maxConcurrent in-flight.
        size_t inFlight = 0;
        for (size_t i = 0; i < d.bulkImportFutures.size(); ++i) {
            if (d.bulkImportFutures[i].valid())
                ++inFlight;
        }
        const size_t nRows = d.bulkImportPreview.Rows.size();
        while (inFlight < static_cast<size_t>(maxConcurrent)) {
            const size_t noPick = nRows;
            size_t pick = noPick;
            for (size_t idx = 0; idx < nRows; ++idx) {
                if (d.bulkImportFutures[idx].valid()) {
                    continue;
                }
                if (idx < d.bulkImportStatus.size() && BulkImportStatusIsTerminal(d.bulkImportStatus[idx])) {
                    continue;
                }
                const auto& scanRow = d.bulkImportPreview.Rows[idx];
                if (!scanRow.Draft.ExistingIssueKey.empty() && scanRow.Error.empty() &&
                    !BulkImportFindTicketInSnapshot(scanRow.Draft.ExistingIssueKey, ticketsSnap.get()) &&
                    app.IsBulkImportPrefetchInFlight(scanRow.Draft.ExistingIssueKey)) {
                    d.bulkImportStatus[idx] = "waiting for cache…";
                    continue;
                }
                pick = idx;
                break;
            }
            if (pick == noPick) {
                break;
            }
            const auto& row = d.bulkImportPreview.Rows[pick];
            if (!row.Error.empty()) {
                d.bulkImportStatus[pick] = "parse error: " + row.Error;
                ++d.bulkImportCompleted;
                continue;
            }
            if (BulkImportRowIsNoopUpdate(row, ticketsSnap.get())) {
                d.bulkImportStatus[pick] = "skipped (no changes)";
                ++d.bulkImportCompleted;
                continue;
            }
            d.bulkImportFutures[pick] = app.CreateIssueAsync(row.Draft);
            d.bulkImportStatus[pick] = "submitting...";
            ++inFlight;
        }
        // Reap.
        for (size_t i = 0; i < d.bulkImportFutures.size(); ++i) {
            auto& fut = d.bulkImportFutures[i];
            if (!fut.valid())
                continue;
            if (fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            IssueCreateResult r = fut.get();
            if (r.Ok) {
                d.bulkImportStatus[i] = "ok " + r.IssueKey;
            } else {
                std::string msg = r.Error.empty() ? "failed" : r.Error;
                if (IsTrackerTransportErrorText(msg)) {
                    msg = "Network/unreachable: " + msg + " — retry when Jira is reachable.";
                }
                if (!r.MissingFieldIds.empty()) {
                    msg += " [missing: ";
                    std::vector<std::string> names = IssueDraftHelpers::MapFieldIdsToNames(r.MissingFieldIds, app.GetAvailableFields());
                    msg += JoinStrings(names, ", ");
                    msg += "]";
                }
                d.bulkImportStatus[i] = msg;
                const auto& bulkRow = d.bulkImportPreview.Rows[i];
                SmatchetToastManager::Instance().Push("Import Error", "Line " + std::to_string(bulkRow.SourceLine) + ": " + msg, ToastType::Error);
            }
            ++d.bulkImportCompleted;
        }
        if (d.bulkImportCompleted >= d.bulkImportPreview.Rows.size()) {
            d.bulkImportRunning = false;
        }
    }

    ImGui::SameLine();
    ImGui::Text("Completed %zu / %zu", d.bulkImportCompleted, d.bulkImportPreview.Rows.size());

    if (ImGui::BeginTable("bulkImportPreview", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable,
                          ImVec2(-FLT_MIN, 260.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Changes", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < d.bulkImportPreview.Rows.size(); ++i) {
            const auto& row = d.bulkImportPreview.Rows[i];
            const bool noopRow = BulkImportRowIsNoopUpdate(row, ticketsSnap.get());
            ImGui::TableNextRow();
            if (noopRow) {
                ImGui::BeginDisabled();
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", row.SourceLine);

            ImGui::TableSetColumnIndex(1);
            if (!row.Draft.ExistingIssueKey.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "update %s", row.Draft.ExistingIssueKey.c_str());
            } else {
                ImGui::Text("create %s / %s", row.Draft.ProjectKey.c_str(),
                            row.Draft.IssueTypeName.empty() ? row.Draft.IssueTypeId.c_str()
                                                            : row.Draft.IssueTypeName.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            const auto sumIt = row.Draft.FieldValues.find("summary");
            ImGui::TextUnformatted(sumIt != row.Draft.FieldValues.end() ? sumIt->second.c_str() : "");

            ImGui::TableSetColumnIndex(3);
            if (!row.Draft.ExistingIssueKey.empty()) {
                const CachedTicket* existing =
                    BulkImportFindTicketInSnapshot(row.Draft.ExistingIssueKey, ticketsSnap.get());
                if (existing) {
                    const auto changes = IssueDraftHelpers::ComputeFieldChanges(row.Draft, *existing);
                    if (changes.empty()) {
                        ImGui::TextDisabled("no changes");
                    } else {
                        ImGui::Text("%zu field(s)", changes.size());
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                            for (const auto& c : changes) {
                                ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "%s", c.FieldId.c_str());
                                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "- %s",
                                                   c.OldValue.empty() ? "-" : c.OldValue.c_str());
                                ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "+ %s",
                                                   c.NewValue.empty() ? "-" : c.NewValue.c_str());
                                ImGui::Separator();
                            }
                            ImGui::PopTextWrapPos();
                            ImGui::EndTooltip();
                        }
                    }
                } else if (app.IsBulkImportPrefetchInFlight(row.Draft.ExistingIssueKey)) {
                    ImGui::TextDisabled("loading…");
                } else {
                    ImGui::TextDisabled("not in cache");
                }
            } else {
                ImGui::TextDisabled("new");
            }

            ImGui::TableSetColumnIndex(4);
            const char* status = (i < d.bulkImportStatus.size()) ? d.bulkImportStatus[i].c_str() : "";
            if (!row.Error.empty() && (i >= d.bulkImportStatus.size() || d.bulkImportStatus[i].empty())) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", row.Error.c_str());
            } else {
                ImGui::TextUnformatted(status);
            }

            if (noopRow) {
                ImGui::EndDisabled();
            }
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "Update rows with no field changes vs the cached ticket are skipped (not sent to Jira). While a "
        "row's ticket is still loading into cache, import waits on that row until the fetch finishes, "
        "then may skip or submit; other rows still run. Keys that never appear in cache are still sent "
        "to Jira.");

    ImGui::End();
}

void SmatchetUI::drawBulkExportWindow(AppController& app, UiDrawSession& d) {
    if (!d.showBulkExport)
        return;

    prepareTopLevelWindow(d, "bulk_export", 720.0f, 480.0f);
    if (!ImGui::Begin("Bulk export tickets", &d.showBulkExport)) {
        ImGui::End();
        return;
    }
    repairTopLevelWindow(d, "bulk_export", 420.0f, 320.0f);

    ImGui::TextUnformatted("Destination path (for Save):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##bulkExportPath", d.bulkExportPathBuf, sizeof(d.bulkExportPathBuf));
    ImGui::SameLine();
    const char* kFormats[] = {"CSV", "TSV", "JSON"};
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("##bulkExportFmt", &d.bulkExportFormatSel, kFormats, IM_ARRAYSIZE(kFormats));

    auto buildText = [&]() -> std::string {
        auto snap = app.GetActiveTicketsSnapshot();
        std::vector<CachedTicket> tickets;
        if (snap) {
            // Match grid copy behavior: union rectangle rows and key-column row picks.
            // Export used to only read RectSel.Rows, so shift-drag / rect-only selection exported all tickets.
            const auto& sel = d.gridState.RectSel;
            std::set<int> allRows;
            if (sel.Active) {
                for (int r = sel.MinRow(); r <= sel.MaxRow(); ++r) {
                    if (r >= 0) {
                        allRows.insert(r);
                    }
                }
            }
            for (int r : sel.Rows) {
                if (r >= 0) {
                    allRows.insert(r);
                }
            }
            if (!allRows.empty()) {
                const bool useSorted = !d.filteredIndices.empty();
                for (int row : allRows) {
                    const size_t logicalRow = static_cast<size_t>(row);
                    size_t ticketIndex = logicalRow;
                    if (useSorted) {
                        if (logicalRow >= d.filteredIndices.size()) {
                            continue;
                        }
                        ticketIndex = d.filteredIndices[logicalRow];
                    }
                    if (ticketIndex < snap->size()) {
                        tickets.push_back((*snap)[ticketIndex]);
                    }
                }
            } else {
                tickets = *snap;
            }
        }
        IssueTableSerializer::Format fmt = IssueTableSerializer::Format::Csv;
        if (d.bulkExportFormatSel == 1)
            fmt = IssueTableSerializer::Format::Tsv;
        else if (d.bulkExportFormatSel == 2)
            fmt = IssueTableSerializer::Format::Json;
        return IssueTableSerializer::SerializeTickets(tickets, {}, fmt);
    };

    if (ImGui::Button("Copy to clipboard")) {
        const std::string text = buildText();
        ImGui::SetClipboardText(text.c_str());
        d.bulkExportFeedback = "Copied " + std::to_string(text.size()) + " bytes.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to file")) {
        const std::string text = buildText();
        std::string err;
        if (WriteEntireFile(d.bulkExportPathBuf, text, err)) {
            d.bulkExportFeedback = "Wrote " + std::to_string(text.size()) + " bytes.";
        } else {
            d.bulkExportFeedback = err;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", d.bulkExportFeedback.c_str());

    ImGui::Separator();
    auto snap = app.GetActiveTicketsSnapshot();
    const size_t total = snap ? snap->size() : 0;
    ImGui::Text("Active tickets in view: %zu", total);
    ImGui::TextDisabled("Exports rows in the current selection (rectangle and/or key-column picks), or all view rows "
                        "when nothing is selected.");
    ImGui::TextDisabled("Headers are field ids; import can read them back.");

    ImGui::End();
}





