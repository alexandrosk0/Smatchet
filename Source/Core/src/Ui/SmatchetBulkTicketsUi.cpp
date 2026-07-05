#include "SmatchetUI.h"

#include "AppController.h"
#include "TrackerHttpUtils.h"
#include "IssueDraft.h"
#include "IssueTableSerializer.h"
#include "ITrackerBackend.h"
#include "Logger.h"
#include "ProjectResolver.h"
#include "SmatchetLocalization.h"
#include "SmatchetProjectPicker.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"
#include "StringUtil.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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
        // ImGui owns Buf only until the next edit; storage lives in UserData (bulkImportTextBuf).
        // cppcheck-suppress autoVariables
        data->Buf = buf->data();
    }
    return 0;
}

bool ReadEntireFile(const std::string& path, std::string& outText, std::string& outError) {
    /* PILLAR2_WORKER_ONLY */ // est-latency: ~50ms — sole caller (line 172) inside app.LaunchBackgroundTask lambda.
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
    auto it = std::find_if(tickets->begin(), tickets->end(), [&](const auto& t) { return t.id == issueKey; });
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

/**
 * WS-A non-blocking clear of the per-row create futures. A create launched via
 * CreateIssueAsync writes to a promise and keeps running in the background pool;
 * destroying its future here does not join, but the abandoned worker would still
 * run the full network create + post-create refresh. Signal cancel (so the worker
 * short-circuits), then move any still-valid futures into the session graveyard
 * (drained at app teardown) instead of destroying them inline — so the UI frame
 * returns within budget (#734) even while a create is in flight. Resets the token
 * for the next run and leaves `bulkImportFutures` empty.
 */
void BulkImportAbandonFutures(UiDrawSession& d) {
    d.bulkImportCancel.Cancel();
    for (auto& f : d.bulkImportFutures) {
        if (f.valid()) {
            d.bulkImportFutureGraveyard.push_back(std::move(f));
        }
    }
    d.bulkImportFutures.clear();
    d.bulkImportCancel.Reset();
}

/** Parse the source text into preview rows + reset per-row status/future tracking. */
void BulkImportRunParse(AppController& app, UiDrawSession& d, const std::string& fallbackProject) {
    const std::string text(d.bulkImportTextBuf.data());
    const IssueTableSerializer::Format fmt = BulkFormatFromIndex(d.bulkImportFormatSel);
    d.bulkImportPreview = IssueTableSerializer::ParseDrafts(text, fmt, app.GetAvailableFields(), fallbackProject,
                                                            d.cfg.DefaultIssueTypeId, d.cfg.DefaultIssueTypeName);
    d.bulkImportStatus.assign(d.bulkImportPreview.Rows.size(), std::string());
    d.bulkImportError = d.bulkImportPreview.Error;
    d.bulkImportCompleted = 0;
    d.bulkImportRunning = false;
    BulkImportAbandonFutures(d);
    d.bulkImportFutures.resize(d.bulkImportPreview.Rows.size());
}

/** Pick the next non-terminal, non-in-flight row to submit; returns nRows when none is ready. */
size_t BulkImportPickNextRow(AppController& app, UiDrawSession& d, const std::vector<CachedTicket>* ticketsSnap,
                             size_t nRows) {
    for (size_t idx = 0; idx < nRows; ++idx) {
        if (d.bulkImportFutures[idx].valid()) {
            continue;
        }
        if (idx < d.bulkImportStatus.size() && BulkImportStatusIsTerminal(d.bulkImportStatus[idx])) {
            continue;
        }
        const auto& scanRow = d.bulkImportPreview.Rows[idx];
        if (!scanRow.Draft.ExistingIssueKey.empty() && scanRow.Error.empty() &&
            !BulkImportFindTicketInSnapshot(scanRow.Draft.ExistingIssueKey, ticketsSnap) &&
            app.IsBulkImportPrefetchInFlight(scanRow.Draft.ExistingIssueKey)) {
            d.bulkImportStatus[idx] = "waiting for cache…";
            continue;
        }
        return idx;
    }
    return nRows;
}

/** Build a human-readable failure status from a create result (transport hint + missing fields). */
std::string BulkImportFormatFailure(AppController& app, const IssueCreateResult& r) {
    std::string msg = r.Error.empty() ? "failed" : r.Error;
    if (IsTrackerTransportErrorText(msg)) {
        msg = "Network/unreachable: " + msg + " — retry when Jira is reachable.";
    }
    if (!r.MissingFieldIds.empty()) {
        msg += " [missing: ";
        std::vector<std::string> names =
            IssueDraftHelpers::MapFieldIdsToNames(r.MissingFieldIds, app.GetAvailableFields());
        msg += JoinStrings(names, ", ");
        msg += "]";
    }
    return msg;
}

/** Submit up to maxConcurrent rows, skipping parse-errors and no-op updates inline. */
void BulkImportSubmitPending(AppController& app, UiDrawSession& d, int maxConcurrent,
                             const std::vector<CachedTicket>* ticketsSnap) {
    size_t inFlight = 0;
    for (size_t i = 0; i < d.bulkImportFutures.size(); ++i) {
        if (d.bulkImportFutures[i].valid())
            ++inFlight;
    }
    const size_t nRows = d.bulkImportPreview.Rows.size();
    while (inFlight < static_cast<size_t>(maxConcurrent)) {
        const size_t pick = BulkImportPickNextRow(app, d, ticketsSnap, nRows);
        if (pick == nRows) {
            break;
        }
        const auto& row = d.bulkImportPreview.Rows[pick];
        if (!row.Error.empty()) {
            d.bulkImportStatus[pick] = "parse error: " + row.Error;
            ++d.bulkImportCompleted;
            continue;
        }
        if (BulkImportRowIsNoopUpdate(row, ticketsSnap)) {
            d.bulkImportStatus[pick] = "skipped (no changes)";
            ++d.bulkImportCompleted;
            continue;
        }
        d.bulkImportFutures[pick] = app.CreateIssueAsync(row.Draft, d.bulkImportCancel);
        d.bulkImportStatus[pick] = "submitting...";
        ++inFlight;
    }
}

/** Reap ready futures, set per-row status, and push a toast on failure. */
void BulkImportReapCompletions(AppController& app, UiDrawSession& d) {
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
            const std::string msg = BulkImportFormatFailure(app, r);
            d.bulkImportStatus[i] = msg;
            const auto& bulkRow = d.bulkImportPreview.Rows[i];
            SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.import_error", "Import Error"),
                                                  SmatchetLocalization::Format("bulk.import_line_error", "Line %d: %s",
                                                                               static_cast<int>(bulkRow.SourceLine),
                                                                               msg.c_str()),
                                                  ToastType::Error);
        }
        ++d.bulkImportCompleted;
    }
}

/** Pump submissions + completions for one frame while an import run is active. */
void BulkImportPumpRun(AppController& app, UiDrawSession& d, int maxConcurrent,
                       const std::vector<CachedTicket>* ticketsSnap) {
    BulkImportSubmitPending(app, d, maxConcurrent, ticketsSnap);
    BulkImportReapCompletions(app, d);
    if (d.bulkImportCompleted >= d.bulkImportPreview.Rows.size()) {
        d.bulkImportRunning = false;
    }
}

/** Reset transient bulk-import state when the window is closed (cancels any in-flight load). */
void BulkImportResetOnClose(UiDrawSession& d) {
    if (d.bulkImportLoadCancel) {
        d.bulkImportLoadCancel->store(true);
    }
    d.bulkImportTextBuf.clear();
    d.bulkImportPathBuf[0] = '\0';
    d.bulkImportFormatSel = 0;
    d.bulkImportPreview = {};
    d.bulkImportStatus.clear();
    BulkImportAbandonFutures(d);
    d.bulkImportCompleted = 0;
    d.bulkImportRunning = false;
    d.bulkImportError.clear();
    d.bulkImportWasOpen = false;
}

/** Path field + Load/Paste/Format-combo toolbar row. */
void DrawBulkImportSourceToolbar(AppController& app, UiDrawSession& d) {
    ImGui::TextUnformatted("Source:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(520);
    ImGui::InputText("##bulkImportPath", d.bulkImportPathBuf, sizeof(d.bulkImportPathBuf));
    ImGui::SameLine();
    // Pillar 2 — file read is deferred to a worker. Files can be megabytes (bulk-import scenarios
    // with 1000s of tickets); the inline ifstream blocked the UI thread. The button disables while
    // in-flight; result posts back via MainThreadDispatcher.
    if (d.bulkImportLoadInFlight) {
        ImGui::BeginDisabled();
        ImGui::Button("Load file");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Loading...");
    } else if (ImGui::Button("Load file")) {
        const std::string capturedPath = d.bulkImportPathBuf;
        d.bulkImportLoadInFlight = true;
        d.bulkImportLoadCancel = std::make_shared<std::atomic<bool>>(false);
        auto cancel = d.bulkImportLoadCancel;
        app.LaunchBackgroundTask([&app, &d, capturedPath, cancel]() {
            std::string text, err;
            const bool ok = ReadEntireFile(capturedPath, text, err);
            app.mainThreadDispatcher.PostToMainThread(
                [&d, ok, text = std::move(text), err = std::move(err), cancel]() mutable {
                    d.bulkImportLoadInFlight = false;
                    if (cancel && cancel->load()) {
                        return;
                    }
                    if (ok) {
                        d.bulkImportTextBuf.assign(text.begin(), text.end());
                        d.bulkImportTextBuf.push_back('\0');
                        d.bulkImportError.clear();
                    } else {
                        d.bulkImportError = err;
                    }
                });
        });
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
}

/** Parse-preview button + the target-project modal it opens when the view has no project scope. */
void DrawBulkImportParseControls(AppController& app, UiDrawSession& d) {
    if (ImGui::Button("Parse preview")) {
        // If the active view has no project scope, ask the user before parsing — the parser
        // needs a fallbackProjectKey for "create" rows that don't carry their own project column.
        const ITrackerBackend* backend = app.GetTrackerBackend();
        const std::string scopeProj =
            backend ? backend->Connectivity().ExtractProjectFromQuery(d.cfg.JqlQuery) : std::string();
        if (scopeProj.empty()) {
            d.bulkImportProjectModalOpen = true;
            d.bulkImportProjectModalChosenKey.clear();
        } else {
            BulkImportRunParse(app, d, scopeProj);
        }
    }

    // Target-project modal. Opened above when the active view has no project clause; the
    // user must pick a project before parse can run.
    if (d.bulkImportProjectModalOpen) {
        ImGui::OpenPopup("##BulkImportProjectModal");
    }
    if (ImGui::BeginPopupModal("##BulkImportProjectModal", &d.bulkImportProjectModalOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            SmatchetLocalization::T("bulkImport.chooseProject.title", "Choose target project for bulk import"));
        ImGui::Separator();
        const std::string backendKind = (d.cfg.TrackerType == "Plane")    ? std::string("Plane")
                                        : (d.cfg.TrackerType == "Linear") ? std::string("Linear")
                                                                          : std::string("Jira");
        const std::string endpoint =
            (d.cfg.TrackerType == "Plane")    ? (d.cfg.PlaneUrl + std::string("|") + d.cfg.PlaneWorkspaceSlug)
            : (d.cfg.TrackerType == "Linear") ? (d.cfg.LinearBaseUrl + std::string("|") + d.cfg.LinearTeamId)
                                              : d.cfg.Domain;
        ImGui::SetNextItemWidth(360.0f);
        SmatchetProjectPicker::Draw("bulk_project", d.bulkImportProjectPickerState, app, backendKind, endpoint,
                                    d.bulkImportProjectModalChosenKey);
        ImGui::Separator();
        if (ImGui::Button(SmatchetLocalization::T("bulkImport.chooseProject.cancel", "Cancel"))) {
            d.bulkImportProjectModalOpen = false;
            d.bulkImportProjectModalChosenKey.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        const bool canConfirm = !d.bulkImportProjectModalChosenKey.empty();
        if (!canConfirm) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(SmatchetLocalization::T("bulkImport.chooseProject.confirm", "Use this project"))) {
            const std::string chosen = d.bulkImportProjectModalChosenKey;
            d.bulkImportProjectModalOpen = false;
            d.bulkImportProjectModalChosenKey.clear();
            ImGui::CloseCurrentPopup();
            BulkImportRunParse(app, d, chosen);
        }
        if (!canConfirm) {
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }

    if (!d.bulkImportError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", d.bulkImportError.c_str());
    }
}

/** Source-text editor + the Run-import button/tooltip and the live completed counter. */
void DrawBulkImportRunControls(UiDrawSession& d, int maxConcurrent) {
    ImGui::Separator();
    ImGui::TextDisabled("Paste / edit source text here (headers = field ids or display names):");
    ImGui::InputTextMultiline("##bulkImportText", d.bulkImportTextBuf.data(), d.bulkImportTextBuf.size(),
                              ImVec2(-FLT_MIN, 160.0f),
                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize,
                              &BulkImportTextResizeCallback, &d.bulkImportTextBuf);

    ImGui::Separator();
    ImGui::Text("Parsed rows: %zu", d.bulkImportPreview.Rows.size());
    ImGui::SameLine();
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
        BulkImportAbandonFutures(d);
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
}

/** Render the per-row changes cell for an update row (field-count + diff tooltip). */
void DrawBulkImportChangesCell(AppController& app, const IssueTableSerializer::ImportRow& row,
                               const std::vector<CachedTicket>* ticketsSnap) {
    const CachedTicket* existing = BulkImportFindTicketInSnapshot(row.Draft.ExistingIssueKey, ticketsSnap);
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
}

/** One preview-table row: index, action, summary, changes cell, status. */
void DrawBulkImportPreviewRow(AppController& app, UiDrawSession& d, size_t i,
                              const std::vector<CachedTicket>* ticketsSnap) {
    const auto& row = d.bulkImportPreview.Rows[i];
    const bool noopRow = BulkImportRowIsNoopUpdate(row, ticketsSnap);
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
                    row.Draft.IssueTypeName.empty() ? row.Draft.IssueTypeId.c_str() : row.Draft.IssueTypeName.c_str());
    }

    ImGui::TableSetColumnIndex(2);
    const auto sumIt = row.Draft.FieldValues.find("summary");
    ImGui::TextUnformatted(sumIt != row.Draft.FieldValues.end() ? sumIt->second.c_str() : "");

    ImGui::TableSetColumnIndex(3);
    if (!row.Draft.ExistingIssueKey.empty()) {
        DrawBulkImportChangesCell(app, row, ticketsSnap);
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

/** The scrollable preview table (header + all parsed rows). */
void DrawBulkImportPreviewTable(AppController& app, UiDrawSession& d, const std::vector<CachedTicket>* ticketsSnap) {
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
            DrawBulkImportPreviewRow(app, d, i, ticketsSnap);
        }
        ImGui::EndTable();
    }
}

} // anonymous namespace

void SmatchetUI::drawBulkImportWindow(AppController& app, UiDrawSession& d) {
    if (!d.showBulkImport) {
        if (d.bulkImportWasOpen) {
            BulkImportResetOnClose(d);
        }
        return;
    }
    d.bulkImportWasOpen = true;

    const bool wantFocus = d.requestBulkImportFocus;
    // 4th arg = wantFocus → SetNextWindowFocus before Begin → activates docked tab. See Log fix.
    prepareTopLevelWindow(d, "bulk_import", 900.0f, 600.0f, wantFocus);
    if (!ImGui::Begin("Bulk import tickets", &d.showBulkImport)) {
        if (wantFocus) {
            d.requestBulkImportFocus = false;
        }
        ImGui::End();
        return;
    }
    repairTopLevelWindow(d, "bulk_import", 520.0f, 360.0f);
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestBulkImportFocus = false;
        LOG_DEBUG("Bulk Import window: focused via menu request");
    }

    if (d.bulkImportTextBuf.empty())
        d.bulkImportTextBuf.assign(1, '\0');

    DrawBulkImportSourceToolbar(app, d);
    DrawBulkImportParseControls(app, d);

    const int maxConcurrent = (std::max)(1, d.cfg.ImportMaxConcurrent);
    DrawBulkImportRunControls(d, maxConcurrent);

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

    if (d.bulkImportRunning) {
        BulkImportPumpRun(app, d, maxConcurrent, ticketsSnap.get());
    }

    ImGui::SameLine();
    ImGui::Text("Completed %zu / %zu", d.bulkImportCompleted, d.bulkImportPreview.Rows.size());

    DrawBulkImportPreviewTable(app, d, ticketsSnap.get());

    ImGui::TextDisabled(
        "Update rows with no field changes vs the cached ticket are skipped (not sent to Jira). While a "
        "row's ticket is still loading into cache, import waits on that row until the fetch finishes, "
        "then may skip or submit; other rows still run. Keys that never appear in cache are still sent "
        "to Jira.");

    ImGui::End();
}

namespace {

// Build the serialised export text for the current grid selection (rectangle + key-column picks, or
// all view rows when nothing is selected) in the chosen CSV/TSV/JSON format. Extracted from the
// buildText lambda in drawBulkExportWindow under the function-size cap; behaviour-identical.
std::string BuildBulkExportText(AppController& app, UiDrawSession& d) {
    auto snap = app.GetActiveTicketsSnapshot();
    std::vector<CachedTicket> tickets;
    if (snap) {
        // Match grid copy behavior: union rectangle rows and key-column row picks.
        // Export used to only read RectSel.Rows, so shift-drag / rect-only selection exported all tickets.
        // Export targets the FOCUSED pane's selection (ADR-0018 focused-pane semantics).
        const GridPane& pane = d.focusedPane();
        const auto& sel = pane.gridState.RectSel;
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
            const bool useSorted = !pane.filteredIndices.empty();
            for (int row : allRows) {
                const size_t logicalRow = static_cast<size_t>(row);
                size_t ticketIndex = logicalRow;
                if (useSorted) {
                    if (logicalRow >= pane.filteredIndices.size()) {
                        continue;
                    }
                    ticketIndex = pane.filteredIndices[logicalRow];
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
}

} // namespace

void SmatchetUI::drawBulkExportWindow(AppController& app, UiDrawSession& d) {
    if (!d.showBulkExport)
        return;

    const bool wantFocus = d.requestBulkExportFocus;
    // 4th arg = wantFocus → SetNextWindowFocus before Begin → activates docked tab. See Log fix.
    prepareTopLevelWindow(d, "bulk_export", 720.0f, 480.0f, wantFocus);
    if (!ImGui::Begin("Bulk export tickets", &d.showBulkExport)) {
        if (wantFocus) {
            d.requestBulkExportFocus = false;
        }
        ImGui::End();
        return;
    }
    repairTopLevelWindow(d, "bulk_export", 420.0f, 320.0f);
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestBulkExportFocus = false;
        LOG_DEBUG("Bulk Export window: focused via menu request");
    }

    ImGui::TextUnformatted("Destination path (for Save):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##bulkExportPath", d.bulkExportPathBuf, sizeof(d.bulkExportPathBuf));
    ImGui::SameLine();
    const char* kFormats[] = {"CSV", "TSV", "JSON"};
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("##bulkExportFmt", &d.bulkExportFormatSel, kFormats, IM_ARRAYSIZE(kFormats));

    if (ImGui::Button("Copy to clipboard")) {
        const std::string text = BuildBulkExportText(app, d);
        ImGui::SetClipboardText(text.c_str());
        d.bulkExportFeedback = "Copied " + std::to_string(text.size()) + " bytes.";
    }
    ImGui::SameLine();
    // Pillar 2 — file write deferred to a worker. Serialised payload can be megabytes for grids
    // with thousands of rows; the inline ofstream blocked the UI thread. Build the text on the
    // UI thread (touches the active snapshot) then hand the path+bytes to a worker.
    if (d.bulkExportSaveInFlight) {
        ImGui::BeginDisabled();
        ImGui::Button("Save to file");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Saving...");
    } else if (ImGui::Button("Save to file")) {
        std::string text = BuildBulkExportText(app, d);
        const std::string capturedPath = d.bulkExportPathBuf;
        const size_t byteCount = text.size();
        d.bulkExportSaveInFlight = true;
        d.bulkExportSaveCancel = std::make_shared<std::atomic<bool>>(false);
        auto cancel = d.bulkExportSaveCancel;
        app.LaunchBackgroundTask([&app, &d, capturedPath, text = std::move(text), byteCount, cancel]() mutable {
            std::string err;
            const bool ok = WriteEntireFile(capturedPath, text, err);
            app.mainThreadDispatcher.PostToMainThread([&d, ok, byteCount, err = std::move(err), cancel]() mutable {
                d.bulkExportSaveInFlight = false;
                if (cancel && cancel->load()) {
                    return;
                }
                if (ok) {
                    d.bulkExportFeedback = "Wrote " + std::to_string(byteCount) + " bytes.";
                } else {
                    d.bulkExportFeedback = err;
                }
            });
        });
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
