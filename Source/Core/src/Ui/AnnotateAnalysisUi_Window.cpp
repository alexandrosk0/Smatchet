#include "AnnotateAnalysisUi_Internal.h"
#include "AnnotateAnalysisUi_Window_detail.h"

#include "AppController.h"
#include "SmatchetDockNodeIds.h"
#include "CodeColorView.h"
#include "CppSyntaxHighlight.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "P4Annotate.h"
#include "SmatchetHelpMarker.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerFieldSchema.h"
#include "Ui/P4ClPreview.h"
#include "Ui/P4vLaunch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace AnnotateInternal;

namespace AnnotateInternal {

bool DrawAnnotateHeaderToolbar(AnnotateDrawCtx& ctx) {
    AppController& app = ctx.App;
    const AnnotateUiThemeColors& theme = ctx.Theme;

    const std::string titleIssue =
        ctx.SelectedJiraIssueKey.empty() ? std::string("(no issue selected)") : ctx.SelectedJiraIssueKey;
    ImGui::Text("Annotate: %s", titleIssue.c_str());
    ImGui::SameLine();

    const ImGuiStyle& st = ImGui::GetStyle();
    const float askW = ImGui::CalcTextSize("Ask AI").x + st.FramePadding.x * 2.f;
    const float exportJsonW = ImGui::CalcTextSize("Export JSON").x + st.FramePadding.x * 2.f;
    const float exportCsvW = ImGui::CalcTextSize("Export CSV").x + st.FramePadding.x * 2.f;
    const float closeW = ImGui::CalcTextSize("Close").x + st.FramePadding.x * 2.f;
    const float rowW = askW + exportJsonW + exportCsvW + closeW + st.ItemSpacing.x * 3.f;
    const float slack = ImGui::GetContentRegionAvail().x - rowW;
    if (slack > 0.f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slack);
    }
    PushAnnotateLinkButtonColors(theme);
    if (ImGui::Button("Ask AI")) {
        const std::string payload = BuildAiExport();
        ImGui::SetClipboardText(payload.c_str());
        if (State().aiUrl[0] != '\0') {
            app.OpenUrl(std::string(State().aiUrl));
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Copy the full annotate export to the clipboard, then open the AI chat URL (if set under "
                          "Preferences → Annotate).");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export JSON")) {
        ImGui::SetClipboardText(BuildAnnotateExportJson().c_str());
        State().lastUiStatus = "Annotate JSON export copied to clipboard.";
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Copy structured annotate export JSON (entries + nearby lines) to clipboard.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) {
        ImGui::SetClipboardText(BuildAnnotateExportCsv().c_str());
        State().lastUiStatus = "Annotate CSV export copied to clipboard.";
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Copy one-row-per-entry annotate export CSV to clipboard.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        PopAnnotateLinkButtonColors();
        if (ctx.WantClose)
            *ctx.WantClose = true;
        return true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Close Annotate.");
    }
    PopAnnotateLinkButtonColors();
    return false;
}

void DrawAnnotateBannerAndStatus(AnnotateDrawCtx& ctx) {
    const TrackerConnectivityBannerForUi jiraBanner = ctx.App.GetTrackerConnectivityBannerForUi(nullptr);
    if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    } else if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Warning) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    if (!State().lastUiStatus.empty()) {
        ImGui::TextWrapped("%s", State().lastUiStatus.c_str());
        ImGui::Separator();
    }
}

namespace {

// Frame-count line + raw/table (or "Show raw callstack…") view toggle button.
void DrawCallstackViewToggle(AnnotateDrawCtx& ctx, bool streamlinedHide) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    ImGui::Text("Callstack Frames: %zu", ctx.NRow);
    const ImGuiStyle& stBtn = ImGui::GetStyle();
    const float padH = stBtn.FramePadding.x * 2.f;
    const float callstackViewBtnW = (std::max)(
        ImGui::CalcTextSize("Show Raw Text").x + padH,
        (std::max)(ImGui::CalcTextSize("Show Table").x + padH, ImGui::CalcTextSize("Show raw callstack…").x + padH));
    if (!streamlinedHide) {
        ImGui::SameLine();
        PushAnnotateLinkButtonColors(theme);
        if (State().showRaw) {
            if (ImGui::Button("Show Table", ImVec2(callstackViewBtnW, 0.f))) {
                ApplyShowRawCallstack(false);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Return to the callstack table. The multiline field above the table becomes "
                                  "editable again (when this tab shows it).");
            }
        } else {
            if (ImGui::Button("Show Raw Text", ImVec2(callstackViewBtnW, 0.f))) {
                ApplyShowRawCallstack(true);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Show the callstack text in a wide, horizontally scrollable box (read-only in this "
                                  "view). Use Show Table to edit the buffer in the smaller field.");
            }
        }
        PopAnnotateLinkButtonColors();
    } else {
        ImGui::SameLine();
        PushAnnotateLinkButtonColors(theme);
        if (ImGui::Button("Show raw callstack…", ImVec2(callstackViewBtnW, 0.f))) {
            ApplyShowRawCallstack(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Reveal the callstack text, raw/table toggle, before changelist, and Process controls "
                              "(compact layout is used when Annotate is opened from the grid until you click this).");
        }
        PopAnnotateLinkButtonColors();
    }
}

// Before-changelist / before-day inputs + Process / Cancel buttons.
void DrawCallstackProcessControls(AnnotateDrawCtx& ctx) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    ImGui::Spacing();
    ImGui::TextDisabled("Annotate options: Settings → Preferences → Annotate.");
    ImGui::SameLine();
    SmatchetHelpMarker::RenderText("Max frames, ignore list, P4 tools, and Jira callstack source are configured "
                                   "under Settings → Preferences → Annotate.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Before changelist");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Optional: run annotate as of this Perforce changelist.");
    }
    ImGui::SameLine();
    SmatchetHelpMarker::RenderText(
        "Optional: run annotate as of this Perforce changelist (each path is passed as `depot/path@CL`).\n\n"
        "Digits only; leave empty for the current head on each path.\n\n"
        "The date control on the right resolves a calendar day to the first submitted changelist on "
        "that day (using `p4 changes -r -m 1 -s submitted` on a server-wide `//...@start,end` range) "
        "and copies the result into this field.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.f);
    ImGui::InputText("##before_cl_optional", State().atClBuf, sizeof(State().atClBuf));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Perforce changelist number only (decimal digits).");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("or day");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Resolve a calendar day to its first submitted changelist.");
    }
    ImGui::SameLine();
    SmatchetHelpMarker::RenderText(
        "Pick a calendar day, then confirm in the calendar popup. The first submitted changelist "
        "on that day (server date window) is written into the field on the left.");
    ImGui::SameLine();
    if (TrackerDateTimeFieldEditor::RenderGenericDatePicker("##annotate_before_day", State().beforeDateIso, false,
                                                            228.f)) {
        ParsedJiraDateTime parsed;
        if (TryParseJiraDateTime(State().beforeDateIso, parsed)) {
            // Pillar 2 — finding #761: the resolve does a slow server-wide `p4 changes` scan; run it
            // off the UI thread (mirror DrawClTooltipAsync) and apply the result when ready. Snapshot
            // the config into a local — never alias UI state across threads.
            const AnnotateAnalysisConfig cfgCopy = State().annotateCfg;
            const int y = parsed.Year;
            const int mo = parsed.Month;
            const int d = parsed.Day;
            State().beforeClResolving = true;
            State().lastUiStatus = "Resolving first submitted changelist for that day...";
            // Issue #1459 — detach a still-pending previous scan before overwriting beforeClFut.
            // The future comes from std::async, so dropping the last reference to an unready state
            // blocks until the scan finishes — a p4-changes-length UI stall (Pillar 2). The reaper
            // in PollDetails drops it once ready, off the hot path (mirror P4ClPreview detach-reap).
            if (State().beforeClFut.valid() &&
                State().beforeClFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                State().detachedBeforeClFuts.push_back(State().beforeClFut);
            }
            State().beforeClFut = std::async(std::launch::async, [cfgCopy, y, mo, d]() {
                                      std::string cl;
                                      std::string err;
                                      // Encode failure as an empty changelist so the UI-thread apply
                                      // branches exactly as the old synchronous code did on `false`.
                                      if (!P4FirstSubmittedChangelistOnCalendarDay(cfgCopy, y, mo, d, cl, err)) {
                                          cl.clear();
                                      }
                                      return std::make_pair(cl, err);
                                  }).share();
        } else {
            State().lastUiStatus = "Invalid date from picker.";
        }
    }

    // Poll the off-thread day→CL resolve (finding #761) and apply on the UI thread once ready.
    if (State().beforeClResolving && State().beforeClFut.valid() &&
        State().beforeClFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        State().beforeClResolving = false;
        const std::pair<std::string, std::string> res = State().beforeClFut.get();
        const std::string& cl = res.first;
        const std::string& err = res.second;
        if (!cl.empty()) {
            std::snprintf(State().atClBuf, sizeof(State().atClBuf), "%s", cl.c_str());
            State().lastUiStatus = "Before changelist set to first submitted CL on that day: " + cl;
        } else {
            State().lastUiStatus = err.empty() ? "Could not resolve changelist for that date." : err;
        }
    }

    PushAnnotateLinkButtonColors(theme);
    if (ImGui::Button("Process") && !ctx.Busy) {
        RunAnnotateProcessFromBuffers();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !ctx.Busy) {
        ImGui::SetTooltip("Parse the callstack buffer and fetch Perforce annotate for each frame (options under "
                          "Preferences → Annotate). If you opened Annotate from the grid with a callstack field, "
                          "this usually runs once automatically after the buffer fills.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") && ctx.Busy) {
        State().worker.Cancel = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ctx.Busy) {
        ImGui::SetTooltip("Stop the in-progress Annotate worker.");
    }
    PopAnnotateLinkButtonColors();
}

// Raw (read-only colored) or multiline editable callstack buffer field.
void DrawCallstackBufferField() {
    if (State().showRaw) {
        float rawMaxLineW = 0.f;
        for (const char* p = State().callstackBuf; *p != '\0';) {
            const char* nl = std::strchr(p, '\n');
            const char* end = nl ? nl : p + std::strlen(p);
            const ImVec2 sz = ImGui::CalcTextSize(p, end);
            rawMaxLineW = std::max(rawMaxLineW, sz.x);
            if (!nl) {
                break;
            }
            p = nl + 1;
        }
        const float cap = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.f;
        const float rawFieldW = std::min(std::max(rawMaxLineW + ImGui::GetStyle().FramePadding.x * 2.f, 120.f), cap);

        ImGui::BeginChild("##rawcs_scroll", ImVec2(rawFieldW, 220.f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        // Slice 7 of code-syntax-coloring-and-tooltips —
        // semantic callstack tokenizer recognises the canonical
        // Module!Class::Method() [Path\File.ext:Line] shape and
        // emits per-element colours (class = Identifier,
        // method = Keyword, filename = Number, lineno =
        // Preprocessor, path = Comment). Falls back to generic
        // cpp coloring on non-callstack rows.
        DrawColoredCallstackText(State().callstackBuf);
        ImGui::EndChild();
    } else {
        ImGui::InputTextMultiline("##callstackpaste", State().callstackBuf, sizeof(State().callstackBuf),
                                  ImVec2(-1.f, 120.f), 0);
    }
}

// Render the "#" (index) cell of one callstack table row, including the row's
// open-Entry-tab click, right-click copy popup, and hover tooltip.
void DrawCallstackRowIndexCell(AnnotateDrawCtx& ctx, size_t i, const AnnotateRow& row) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::TableSetColumnIndex(0);
    char idxBuf[16];
    std::snprintf(idxBuf, sizeof(idxBuf), "%u", static_cast<unsigned>(i + 1));
    PushAnnotateLinkTextOnly(theme);
    ImGui::SelectableRaw(idxBuf, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                         ImVec2(0.f, rowH));
    PopAnnotateLinkTextOnly();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        State().pendingSelectEntryIndex = static_cast<int>(i);
        EnsureDetailLoading(i, State().annotateCfg, std::string(State().atClBuf));
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
        ImGui::OpenPopup("annotate_cs_row_copy");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Left-click: open the Entry tab for this frame and load its annotation.\n"
                          "Right-click: menu with Copy (this row as TSV).");
    }
    if (ImGui::BeginPopup("annotate_cs_row_copy")) {
        if (ImGui::MenuItem("Copy")) {
            ImGui::SetClipboardText(BuildCallstackRowTsv(row, i).c_str());
        }
        ImGui::EndPopup();
    }
}

// Function (col 1) + Location (col 2) cells: copy-name / open-timelapse links.
void DrawCallstackRowFnLocCells(AnnotateDrawCtx& ctx, const AnnotateRow& row) {
    const AnnotateUiThemeColors& theme = ctx.Theme;

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemAllowOverlap();
    {
        const char* fn = row.Parsed.Function.c_str();
        const float colAvail = ImGui::GetContentRegionAvail().x;
        const float lineH = ImGui::GetTextLineHeight();
        PushAnnotateLinkTextOnly(theme);
        if (ImGui::SelectableRaw(fn, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(colAvail, lineH))) {
            /* click handled below */
        }
        PopAnnotateLinkTextOnly();
        if (ImGui::IsItemClicked()) {
            ImGui::SetClipboardText(fn);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Left-click: copy the function name to the clipboard.\n\n%s", fn);
        }
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::SetNextItemAllowOverlap();
    {
        const std::string fullLoc = row.PathForP4 + ":" + std::to_string(row.Parsed.LineNumber);
        const float locCellW = ImGui::GetColumnWidth();
        const std::string shortPath = ShortenPathForDisplay(row.PathForP4, std::max(32.f, locCellW - 40.f));
        const std::string shortLoc = shortPath + ":" + std::to_string(row.Parsed.LineNumber);
        PushAnnotateLinkTextOnly(theme);
        if (ImGui::SelectableRaw(shortLoc.c_str(), false, ImGuiSelectableFlags_AllowOverlap)) {
            P4vLaunch::LaunchP4VcLike(State().annotateCfg, State().timeTpl, State().changeTpl, true, row.PathForP4,
                                      row.Parsed.LineNumber, row.Annotate.Changelist);
        }
        PopAnnotateLinkTextOnly();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Left-click: open p4vc timelapse for this file and line.\n\n%s", fullLoc.c_str());
        }
    }
}

// User (col 3) cell: Jira lookup link / assign popup trigger.
void DrawCallstackRowUserCell(AnnotateDrawCtx& ctx, const AnnotateRow& row, bool pending) {
    AppController& app = ctx.App;
    const AnnotateUiThemeColors& theme = ctx.Theme;
    ImGui::TableSetColumnIndex(3);
    ImGui::SetNextItemAllowOverlap();
    const std::string userDisp = AnnotateUiPure::PendingOrDash(pending, row.Annotate.User);
    const bool userActionable = AnnotateUiPure::RowUserIsActionable(pending, row.Annotate.User);
    const bool userDisabled = AnnotateUiPure::RowUserCellDisabled(pending, row.Annotate.User);
    if (userDisabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
    } else {
        PushAnnotateLinkTextOnly(theme);
    }
    ImGui::SelectableRaw((userDisp + "##user").c_str(), false, ImGuiSelectableFlags_AllowOverlap);
    if (userDisabled) {
        ImGui::PopStyleColor();
    } else {
        PopAnnotateLinkTextOnly();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (pending) {
            ImGui::SetTooltip("Waiting for Annotate results for this row.");
        } else if (userActionable) {
            ImGui::SetTooltip("Left-click: look up this Perforce user in Jira.\n"
                              "Right-click: open the assign dialog for this row.\n%s",
                              row.Annotate.Approximate ? "\nApproximate Annotate (line may not match exact CL)." : "");
        } else {
            ImGui::SetTooltip("No Perforce user on this row.");
        }
    }
    if (!pending && ImGui::IsItemClicked() && !row.Annotate.User.empty() && row.Annotate.User != "...") {
        OpenTrackerUserProfileForP4User(app, row.Annotate.User);
    }
    if (ImGui::IsMouseClicked(1) && ImGui::IsItemHovered()) {
        PrepareAssignModal(app, row, pending ? std::string() : row.Annotate.User);
        State().openAssignModal = true;
    }
}

// CL (col 4) + Date (col 5) cells of one callstack row.
void DrawCallstackRowClDateCells(AnnotateDrawCtx& ctx, const AnnotateRow& row, bool pending) {
    const AnnotateUiThemeColors& theme = ctx.Theme;

    ImGui::TableSetColumnIndex(4);
    ImGui::SetNextItemAllowOverlap();
    {
        const std::string clDisp = AnnotateUiPure::PendingOrDash(pending, row.Annotate.Changelist);
        const bool clDisabled = AnnotateUiPure::RowClCellDisabled(pending, row.Annotate.Changelist);
        if (clDisabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
        } else {
            PushAnnotateLinkTextOnly(theme);
        }
        ImGui::SelectableRaw((clDisp + "##cl").c_str(), false, ImGuiSelectableFlags_AllowOverlap);
        if (clDisabled) {
            ImGui::PopStyleColor();
        } else {
            PopAnnotateLinkTextOnly();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            if (pending) {
                ImGui::SetTooltip("Waiting for Annotate results for this row.");
            } else if (row.Annotate.Changelist.empty()) {
                ImGui::SetTooltip("No changelist on this row.");
            } else {
                P4ClPreview::DrawClTooltipAsync(row.Annotate.Changelist, State().annotateCfg, theme);
            }
        }
        if (ImGui::IsItemClicked() && !pending && !row.Annotate.Changelist.empty()) {
            P4vLaunch::LaunchP4VcLike(State().annotateCfg, State().timeTpl, State().changeTpl, false, row.PathForP4,
                                      row.Parsed.LineNumber, row.Annotate.Changelist);
        }
    }

    ImGui::TableSetColumnIndex(5);
    ImGui::SetNextItemAllowOverlap();
    if (pending) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
        ImGui::TextUnformatted("-");
        ImGui::PopStyleColor();
    } else if (row.Annotate.Date.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
        ImGui::TextUnformatted("-");
        ImGui::PopStyleColor();
    } else {
        const std::string dd = NormalizeDateDisplay(row.Annotate.Date);
        ImGui::TextUnformatted(dd.c_str());
    }
}

// The full 6-column callstack table inside its dedicated scroll child.
void DrawCallstackTable(AnnotateDrawCtx& ctx) {
    // Keep Process / callstack editor above a dedicated scroll region. A tall in-table
    // ScrollY made the whole tab body exceed the viewport so the window scroll bar hid
    // the controls at the top (Process looked "missing").
    const float tblScrollH = std::max(ImGui::GetContentRegionAvail().y - 4.f, 80.f);
    ImGui::BeginChild("##annotate_callstack_tbl_scroll", ImVec2(0.f, tblScrollH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    const float locColW = 250.f;
    if (ImGui::BeginTable("annotate_tbl", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
                          ImVec2(-1.f, 0.f))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.f, 0);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 0.f, 1);
        ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, locColW, 2);
        ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 120.f, 3);
        ImGui::TableSetupColumn("CL", ImGuiTableColumnFlags_WidthFixed, 70.f, 4);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 90.f, 5);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < ctx.NRow; ++i) {
            const AnnotateRow& row = ctx.RowsSnap[i];
            const bool pending = ctx.Busy && static_cast<int>(i) >= ctx.Prog;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            DrawCallstackRowIndexCell(ctx, i, row);
            DrawCallstackRowFnLocCells(ctx, row);
            DrawCallstackRowUserCell(ctx, row, pending);
            DrawCallstackRowClDateCells(ctx, row, pending);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace

void DrawAnnotateCallstackTab(AnnotateDrawCtx& ctx) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    const bool streamlinedHide = State().annotateStreamlinedFromGrid && !State().showRaw;

    DrawCallstackViewToggle(ctx, streamlinedHide);

    if (!State().showRaw && ctx.NRow > 0) {
        ImGui::TextDisabled("Table: # column opens an Entry tab.");
        ImGui::SameLine();
        SmatchetHelpMarker::RenderText(
            "Left-click the # column to open that frame in an Entry tab; right-click # for a menu "
            "(Copy as TSV).");
    }

    if (!streamlinedHide) {
        DrawCallstackProcessControls(ctx);
    } else if (ctx.Busy) {
        PushAnnotateLinkButtonColors(theme);
        if (ImGui::Button("Cancel") && ctx.Busy) {
            State().worker.Cancel = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Stop the in-progress Annotate worker.");
        }
        PopAnnotateLinkButtonColors();
    }

    if (!streamlinedHide) {
        DrawCallstackBufferField();
    }

    if (!State().showRaw && ctx.NRow > 0) {
        DrawCallstackTable(ctx);
    }
}

namespace {

// Shared row-cell affordances reused after each annotate-line cell, acting on the
// last-submitted item (immediate-mode). Kept verbatim from the monolith's two
// per-row lambdas so each cell stays independently right-clickable / hoverable.
void AnnRowOpenCopyMenu() {
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
        ImGui::OpenPopup("annotate_ann_row_copy");
    }
}
void AnnRowHoverTip() {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Right-click any cell in this row for a menu: Copy (full line as TSV). Assign... "
                          "appears only when the Perforce user can be used for assign (not '-' or '...').");
    }
}

// State carried through the per-line annotate-table loop so each cell helper reads
// the same theme / highlight color / target line / hit-height without re-deriving.
struct EntryLineCtx {
    AppController& App;
    const AnnotateUiThemeColors& Theme;
    const AnnotateRow& Row;
    int TargetLine;
    float HitH;
};

// Cols 0 (highlight mark) + 1 (line number): selectable hit targets only.
void DrawEntryLineMarkAndLineCells(const EntryLineCtx& lc, const P4AnnotatedLine& ln, bool hl) {
    const AnnotateUiThemeColors& theme = lc.Theme;

    ImGui::TableSetColumnIndex(0);
    {
        const float markW = ImGui::GetContentRegionAvail().x;
        if (hl) {
            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.StatusWarning));
        }
        PushAnnotateLinkTextOnly(theme);
        ImGui::SelectableRaw(hl ? ">>>" : " ", false, ImGuiSelectableFlags_AllowOverlap,
                             ImVec2((std::max)(markW, 1.f), lc.HitH));
        PopAnnotateLinkTextOnly();
        if (hl) {
            ImGui::PopStyleColor();
        }
        AnnRowOpenCopyMenu();
        AnnRowHoverTip();
    }

    ImGui::TableSetColumnIndex(1);
    {
        char lineBuf[32];
        std::snprintf(lineBuf, sizeof(lineBuf), "%d", ln.SourceLine);
        const float lineColW = ImGui::GetContentRegionAvail().x;
        PushAnnotateLinkTextOnly(theme);
        ImGui::SelectableRaw(lineBuf, false, ImGuiSelectableFlags_AllowOverlap,
                             ImVec2((std::max)(lineColW, 1.f), lc.HitH));
        PopAnnotateLinkTextOnly();
        AnnRowOpenCopyMenu();
        AnnRowHoverTip();
    }
}

// Col 2: changelist cell (dim "-" or p4vc-launch link + CL tooltip).
void DrawEntryLineClCell(const EntryLineCtx& lc, const P4AnnotatedLine& ln) {
    const AnnotateUiThemeColors& theme = lc.Theme;
    ImGui::TableSetColumnIndex(2);
    ImGui::SetNextItemAllowOverlap();
    const float clCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
    if (ln.Changelist.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
        ImGui::SelectableRaw("-##cl", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(clCellW, lc.HitH));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("No changelist on this line. Right-click for row menu (Copy).");
        }
    } else {
        PushAnnotateLinkTextOnly(theme);
        if (ImGui::SelectableRaw(ln.Changelist.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                 ImVec2(clCellW, lc.HitH))) {
            P4vLaunch::LaunchP4VcLike(State().annotateCfg, State().timeTpl, State().changeTpl, false, lc.Row.PathForP4,
                                      ln.SourceLine, ln.Changelist);
        }
        PopAnnotateLinkTextOnly();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            P4ClPreview::DrawClTooltipAsync(ln.Changelist, State().annotateCfg, theme);
        }
    }
    AnnRowOpenCopyMenu();
}

// Col 3: user cell (dim "-" or Jira-lookup link with approximate-row tooltip note).
void DrawEntryLineUserCell(const EntryLineCtx& lc, const P4AnnotatedLine& ln, bool hl) {
    const AnnotateUiThemeColors& theme = lc.Theme;
    ImGui::TableSetColumnIndex(3);
    ImGui::SetNextItemAllowOverlap();
    const float userCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
    if (ln.User.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
        ImGui::SelectableRaw("-##user", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(userCellW, lc.HitH));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            std::string tip = "No Perforce user on this line. Right-click for row menu (Copy).";
            if (hl && lc.Row.Annotate.Approximate) {
                tip += "\n\nApproximate row: unable to find exact CL and user.";
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    } else {
        PushAnnotateLinkTextOnly(theme);
        ImGui::SelectableRaw(ln.User.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(userCellW, lc.HitH));
        PopAnnotateLinkTextOnly();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            std::string tip = "Left-click: look up this Perforce user in Jira.\n"
                              "Right-click: row menu - Copy (full line as TSV); Assign... when the "
                              "user is eligible (not '-' or '...').";
            if (hl && lc.Row.Annotate.Approximate) {
                tip += "\n\nApproximate row: unable to find exact CL and user.";
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            OpenTrackerUserProfileForP4User(lc.App, ln.User);
        }
    }
    AnnRowOpenCopyMenu();
}

// Col 4 (date) + col 5 (colored code + RMB hit target).
void DrawEntryLineDateAndCodeCells(const EntryLineCtx& lc, const P4AnnotatedLine& ln) {
    const AnnotateUiThemeColors& theme = lc.Theme;

    ImGui::TableSetColumnIndex(4);
    const float dateCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
    if (ln.Date.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
        ImGui::SelectableRaw("-##date", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(dateCellW, lc.HitH));
        ImGui::PopStyleColor();
    } else {
        const std::string dd = NormalizeDateDisplay(ln.Date);
        ImGui::SelectableRaw(dd.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(dateCellW, lc.HitH));
    }
    AnnRowOpenCopyMenu();
    AnnRowHoverTip();

    ImGui::TableSetColumnIndex(5);
    {
        const float codeColW = ImGui::GetContentRegionAvail().x;
        const ImVec2 codeCell0 = ImGui::GetCursorScreenPos();
        // Slice 5 of code-syntax-coloring-and-tooltips —
        // route the annotate line through CodeColorView with
        // language inferred from the file being annotated. .lua /
        // .py / .glsl files now render with their respective
        // LDs instead of C++-only.
        smatchet::code_color::DrawColoredCode(ln.Code.c_str(),
                                              smatchet::code_color::LangFromFilePath(lc.Row.PathForP4));
        ImGui::SetCursorScreenPos(codeCell0);
        ImGui::SelectableRaw("##ann_code_rmb", false, ImGuiSelectableFlags_AllowOverlap,
                             ImVec2((std::max)(codeColW, 1.f), lc.HitH));
        AnnRowOpenCopyMenu();
        AnnRowHoverTip();
    }
}

// Right-click row context menu (Copy / Assign...) for one annotated line.
void DrawEntryLineCopyMenu(const EntryLineCtx& lc, const P4AnnotatedLine& ln) {
    if (ImGui::BeginPopup("annotate_ann_row_copy")) {
        if (ImGui::MenuItem("Copy")) {
            ImGui::SetClipboardText(BuildAnnotatedRowTsv(ln).c_str());
        }
        const bool canAssign = AnnotateUiPure::LineCanAssign(ln.User);
        if (canAssign) {
            if (ImGui::MenuItem("Assign...")) {
                AnnotateRow br = lc.Row;
                br.Annotate.User = ln.User;
                br.Annotate.Changelist = ln.Changelist;
                br.Parsed.LineNumber = ln.SourceLine;
                br.Annotate.LineSnippet = ln.Code;
                br.Annotate.Date = ln.Date;
                br.Annotate.Approximate = false;
                PrepareAssignModal(lc.App, br, ln.User);
                ImGui::CloseCurrentPopup();
                State().openAssignModal = true;
            }
        }
        ImGui::EndPopup();
    }
}

// The 6-column annotated-source table for one Entry tab.
void DrawEntryAnnotateTable(AnnotateDrawCtx& ctx, size_t ti, const AnnotateRow& row) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 30.f);
    ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 60.f);
    ImGui::TableSetupColumn("CL", ImGuiTableColumnFlags_WidthFixed, 70.f);
    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 120.f);
    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 100.f);
    ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const int targetLine = row.Parsed.LineNumber;
    const std::vector<P4AnnotatedLine>& lines = State().detailData[ti].Lines;
    const float annRowHitH = ImGui::GetTextLineHeightWithSpacing();
    const ImU32 hlU32 = ImGui::ColorConvertFloat4ToU32(ThCol(theme.FindHighlight));
    EntryLineCtx lc{ctx.App, theme, row, targetLine, annRowHitH};
    for (size_t lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        const P4AnnotatedLine& ln = lines[lineIdx];
        ImGui::PushID(static_cast<int>(lineIdx));
        ImGui::TableNextRow();
        const bool hl = (ln.SourceLine == targetLine);
        if (hl) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, hlU32);
        }

        DrawEntryLineMarkAndLineCells(lc, ln, hl);
        DrawEntryLineClCell(lc, ln);
        DrawEntryLineUserCell(lc, ln, hl);
        DrawEntryLineDateAndCodeCells(lc, ln);
        DrawEntryLineCopyMenu(lc, ln);

        if (hl && ti < State().detailScrolled.size() && !State().detailScrolled[ti]) {
            ImGui::SetScrollHereY(0.5f);
            State().detailScrolled[ti] = true;
        }

        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace

void DrawAnnotateEntryTab(AnnotateDrawCtx& ctx, size_t ti) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    EnsureDetailLoading(ti, State().annotateCfg, std::string(State().atClBuf));
    AnnotateRow row = ctx.RowsSnap[ti];
    ImGui::Text("File: %s", row.PathForP4.c_str());
    ImGui::Text("Target Line: %d", row.Parsed.LineNumber);

    int phase = 0;
    if (ti < State().detailPhase.size()) {
        phase = State().detailPhase[ti];
    }
    if (phase == 1) {
        ImGui::TextUnformatted("Loading annotated file...");
    } else if (ti < State().detailData.size() && !State().detailData[ti].Error.empty()) {
        ImGui::TextColored(ThCol(theme.StatusError), "%s", State().detailData[ti].Error.c_str());
    } else if (ti < State().detailData.size() &&
               ImGui::BeginTable("ann", 6,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                     ImGuiTableFlags_ScrollY,
                                 ImVec2(0, ImGui::GetContentRegionAvail().y - 8.f))) {
        DrawEntryAnnotateTable(ctx, ti, row);
    }
}

namespace {

// "Assign issue to user" action row (off-UI SubmitFieldEdit dispatch).
void DrawAssignIssueAction(AnnotateDrawCtx& ctx, bool readOnlyMode, bool commitInFlight, bool hasJiraAccount) {
    AppController& app = ctx.App;
    const std::string& selectedJiraIssueKey = ctx.SelectedJiraIssueKey;
    ImGui::BeginDisabled(readOnlyMode || commitInFlight);
    if (ImGui::Selectable("Assign issue to user", false)) {
        const TrackerField* f = app.FindFieldById("assignee");
        if (!hasJiraAccount) {
            LOG_ERROR("Annotate UI: assign skipped — no Jira account match for this Perforce user.");
            State().lastUiStatus = "No Jira user match for assign.";
        } else if (!f) {
            LOG_ERROR("Annotate UI: assignee field not in catalog.");
            State().lastUiStatus = "assignee field not in catalog.";
        } else {
            // Pillar 2 — finding #7: dispatch SubmitFieldEdit (cpr::Post) off the UI thread.
            State().assignCommitInFlight = true;
            State().lastUiStatus = "Posting...";
            const std::string capturedIssueKey = selectedJiraIssueKey;
            const std::string capturedAccountId = State().assignAccountId;
            const TrackerField fieldCopy = *f;
            app.LaunchBackgroundTask([&app, capturedIssueKey, capturedAccountId, fieldCopy]() {
                std::string err;
                const bool ok = app.SubmitFieldEdit(capturedIssueKey, fieldCopy, {capturedAccountId}, err);
                app.PostToMainThread([ok, err, capturedIssueKey]() {
                    if (!HasLiveStateInstance()) {
                        return;
                    }
                    State().assignCommitInFlight = false;
                    if (ok) {
                        LOG_INFO("Annotate UI: assignee set on %s", capturedIssueKey.c_str());
                        State().lastUiStatus = "Assignee updated.";
                    } else {
                        LOG_ERROR("Annotate UI: assign failed: %s", err.c_str());
                        State().lastUiStatus = err;
                    }
                });
            });
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
}

// "Add Annotate context comment" action row (off-UI AddIssueCommentAnnotateContext).
void DrawAssignContextCommentAction(AnnotateDrawCtx& ctx, bool readOnlyMode, bool commitInFlight) {
    AppController& app = ctx.App;
    const std::string& selectedJiraIssueKey = ctx.SelectedJiraIssueKey;
    ImGui::BeginDisabled(readOnlyMode || commitInFlight);
    if (ImGui::Selectable("Add Annotate context comment", false)) {
        // Pillar 2 — finding #7: AddIssueCommentAnnotateContext (cpr::Post) → worker.
        State().assignCommitInFlight = true;
        State().lastUiStatus = "Posting...";
        const std::string capturedIssueKey = selectedJiraIssueKey;
        const AnnotateRow capturedRow = State().assignRow;
        app.LaunchBackgroundTask([&app, capturedIssueKey, capturedRow]() {
            std::string err;
            const bool ok = app.AddIssueCommentAnnotateContext(
                capturedIssueKey, capturedRow.Annotate.User, capturedRow.Parsed.Function, capturedRow.PathForP4,
                capturedRow.Parsed.LineNumber, capturedRow.Annotate.Changelist, capturedRow.Annotate.Date,
                capturedRow.Annotate.Approximate, capturedRow.Annotate.LineSnippet, err);
            app.PostToMainThread([ok, err, capturedIssueKey]() {
                if (!HasLiveStateInstance()) {
                    return;
                }
                State().assignCommitInFlight = false;
                if (ok) {
                    LOG_INFO("Annotate UI: posted annotate context comment for %s.", capturedIssueKey.c_str());
                    State().lastUiStatus = "Annotate context comment posted.";
                } else {
                    LOG_ERROR("Annotate UI: comment failed: %s", err.c_str());
                    State().lastUiStatus = err;
                }
            });
        });
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
}

// "Quick comment templates" selectable list (off-UI AddIssueCommentPlain per template).
void DrawAssignQuickCommentTemplates(AnnotateDrawCtx& ctx, const TrackerConfig& jiraCfg, bool readOnlyMode,
                                     bool commitInFlight) {
    AppController& app = ctx.App;
    const std::string& selectedJiraIssueKey = ctx.SelectedJiraIssueKey;
    ImGui::Separator();
    ImGui::TextDisabled("Quick comment templates");
    ImGui::BeginDisabled(readOnlyMode || commitInFlight);
    {
        int annotateTplIndex = 0;
        for (const auto& t : jiraCfg.AnnotateCommentTemplates) {
            if (!t.Id.empty()) {
                ImGui::PushID(t.Id.c_str());
            } else {
                ImGui::PushID(annotateTplIndex);
            }
            if (ImGui::SelectableRaw(t.Title.c_str(), false)) {
                // Pillar 2 — finding #7: AddIssueCommentPlain (cpr::Post) → worker.
                State().assignCommitInFlight = true;
                State().lastUiStatus = "Posting...";
                const std::string capturedIssueKey = selectedJiraIssueKey;
                const std::string capturedTitle = t.Title;
                const std::string commentBody = BuildAnnotateQuickCommentTemplate(
                    selectedJiraIssueKey, t.Id, State().assignRow, jiraCfg.AnnotateCommentTemplates);
                app.LaunchBackgroundTask([&app, capturedIssueKey, capturedTitle, commentBody]() {
                    std::string err;
                    const bool ok = app.AddIssueCommentPlain(capturedIssueKey, commentBody, err);
                    app.PostToMainThread([ok, err, capturedTitle]() {
                        if (!HasLiveStateInstance()) {
                            return;
                        }
                        State().assignCommitInFlight = false;
                        if (ok) {
                            State().lastUiStatus = "Posted '" + capturedTitle + "' comment.";
                        } else {
                            State().lastUiStatus = err.empty() ? "Failed to post Jira comment." : err;
                        }
                    });
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
            ++annotateTplIndex;
        }
    }
    ImGui::EndDisabled();
    if (readOnlyMode && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Disabled while Jira is in read-only mode.");
    }
}

// "Assign and add Annotate context" combined action (chained off-UI calls).
void DrawAssignAndContextAction(AnnotateDrawCtx& ctx, bool readOnlyMode, bool commitInFlight, bool hasJiraAccount) {
    AppController& app = ctx.App;
    const AnnotateUiThemeColors& theme = ctx.Theme;
    const std::string& selectedJiraIssueKey = ctx.SelectedJiraIssueKey;
    if (hasJiraAccount) {
        PushAnnotateLinkTextOnly(theme);
    }
    ImGui::BeginDisabled(!hasJiraAccount || readOnlyMode || commitInFlight);
    if (ImGui::Selectable("Assign and add Annotate context", false)) {
        const TrackerField* f = app.FindFieldById("assignee");
        if (!f) {
            State().lastUiStatus = "assignee field not in catalog.";
        } else {
            // Pillar 2 — finding #7: chain SubmitFieldEdit + AddIssueCommentAnnotateContext
            // on a single worker so the user clicks once and both HTTP calls run off-UI.
            State().assignCommitInFlight = true;
            State().lastUiStatus = "Posting...";
            const std::string capturedIssueKey = selectedJiraIssueKey;
            const std::string capturedAccountId = State().assignAccountId;
            const TrackerField fieldCopy = *f;
            const AnnotateRow capturedRow = State().assignRow;
            app.LaunchBackgroundTask([&app, capturedIssueKey, capturedAccountId, fieldCopy, capturedRow]() {
                std::string err;
                const bool assigned = app.SubmitFieldEdit(capturedIssueKey, fieldCopy, {capturedAccountId}, err);
                bool commented = false;
                if (assigned) {
                    commented = app.AddIssueCommentAnnotateContext(
                        capturedIssueKey, capturedRow.Annotate.User, capturedRow.Parsed.Function, capturedRow.PathForP4,
                        capturedRow.Parsed.LineNumber, capturedRow.Annotate.Changelist, capturedRow.Annotate.Date,
                        capturedRow.Annotate.Approximate, capturedRow.Annotate.LineSnippet, err);
                }
                const bool ok = assigned && commented;
                app.PostToMainThread([ok, err, capturedIssueKey]() {
                    if (!HasLiveStateInstance()) {
                        return;
                    }
                    State().assignCommitInFlight = false;
                    if (ok) {
                        LOG_INFO("Annotate UI: assigned %s and posted annotate context comment.",
                                 capturedIssueKey.c_str());
                        State().lastUiStatus = "Assigned and commented.";
                    } else {
                        LOG_ERROR("Annotate UI: assign/comment failed: %s", err.c_str());
                        State().lastUiStatus = err;
                    }
                });
            });
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    if (hasJiraAccount) {
        PopAnnotateLinkTextOnly();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (hasJiraAccount) {
            ImGui::SetTooltip("Assign the issue, then add a Jira comment summarizing Annotate context "
                              "(user, function, path, line, CL, date).");
        } else {
            ImGui::SetTooltip("Enable this action by matching the Perforce user to a Jira account.");
        }
    }
}

// The assign-modal body shown once a Jira issue is selected (all action rows).
void DrawAssignModalBody(AnnotateDrawCtx& ctx, const TrackerConfig& cfg, bool readOnlyMode) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    const bool hasJiraAccount = State().assignHasJiraAccount && !State().assignAccountId.empty();
    const bool commitInFlight = State().assignCommitInFlight;
    PushAnnotateLinkTextOnly(theme);
    DrawAssignIssueAction(ctx, readOnlyMode, commitInFlight, hasJiraAccount);
    PopAnnotateLinkTextOnly();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "Set the Jira assignee on the selected issue to the Jira user matched from the Perforce user on "
            "the Annotate row you used (callstack table or Entry tab row menu).\n"
            "Requires a matching Jira account; otherwise an error is shown.");
    }
    DrawAssignContextCommentAction(ctx, readOnlyMode, commitInFlight);
    DrawAssignQuickCommentTemplates(ctx, cfg, readOnlyMode, commitInFlight);
    DrawAssignAndContextAction(ctx, readOnlyMode, commitInFlight, hasJiraAccount);
}

} // namespace

void DrawAnnotateAssignModal(AnnotateDrawCtx& ctx) {
    AppController& app = ctx.App;
    const AnnotateUiThemeColors& theme = ctx.Theme;
    if (State().openAssignModal) {
        ImGui::OpenPopup("annotate_assign");
        State().openAssignModal = false;
    }
    if (!ImGui::BeginPopupModal("annotate_assign", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const TrackerConnectivityBannerForUi jiraBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
    const TrackerConfig cfg = ConfigManager::Load();
    const bool readOnlyMode = cfg.ReadOnlyMode || (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Error);
    ImGui::TextUnformatted(State().assignTitle.c_str());
    ImGui::Separator();
    if (cfg.ReadOnlyMode) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.22f, 1.0f));
        ImGui::TextWrapped("Read-only mode is enabled in Preferences.");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Assign and comment actions stay disabled until read-only mode is turned off.");
        ImGui::Separator();
    }
    if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Assign and comment actions stay disabled until Jira is reachable.");
        ImGui::Separator();
    } else if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Warning) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    if (ctx.SelectedJiraIssueKey.empty()) {
        ImGui::TextDisabled("Select a Jira issue in the grid.");
    } else {
        DrawAssignModalBody(ctx, cfg, readOnlyMode);
    }
    PushAnnotateLinkButtonColors(theme);
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Close this dialog without making further changes.");
    }
    PopAnnotateLinkButtonColors();
    ImGui::EndPopup();
}

void DrawAnnotateProfileModal(AnnotateDrawCtx& ctx) {
    const AnnotateUiThemeColors& theme = ctx.Theme;
    if (State().openProfileModal) {
        ImGui::OpenPopup("Jira user profile");
        State().openProfileModal = false;
    }
    if (!ImGui::BeginPopupModal("Jira user profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (!State().profileErr.empty() && State().profileName.empty()) {
        ImGui::TextUnformatted(State().profileErr.c_str());
    } else {
        ImGui::Text("Name: %s", State().profileName.c_str());
        ImGui::Text("Email: %s", State().profileEmail.c_str());
        if (!State().profileErr.empty()) {
            ImGui::TextDisabled("%s", State().profileErr.c_str());
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Groups (best effort):");
        if (State().profileGroups.empty()) {
            ImGui::TextDisabled("(none or not permitted)");
        } else {
            for (const auto& gname : State().profileGroups) {
                ImGui::BulletText("%s", gname.c_str());
            }
        }
    }
    PushAnnotateLinkButtonColors(theme);
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Close the Jira user profile dialog.");
    }
    PopAnnotateLinkButtonColors();
    ImGui::EndPopup();
}

} // namespace AnnotateInternal

void AnnotateAnalysisUi::DrawWindow(AppController& app, bool* pOpen, const std::string& selectedJiraIssueKey) {
    if (!pOpen || !*pOpen) {
        if (pOpen)
            CloseAnnotateModal(pOpen);
        return;
    }
    constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
        ImGui::SetNextWindowDockID(SmatchetDockNodeIds::kBottomPanel, ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Annotate###AnnotateAnalysisModal", pOpen, kPanelFlags)) {
        if (!*pOpen) {
            CloseAnnotateModal(pOpen);
        }
        ImGui::End();
        return;
    }
    if (!*pOpen) {
        CloseAnnotateModal(pOpen);
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        CloseAnnotateModal(pOpen);
        ImGui::End();
        return;
    }
    bool wantClose = false;
    DrawContent(app, &wantClose, selectedJiraIssueKey);
    if (wantClose)
        CloseAnnotateModal(pOpen);
    ImGui::End();
}

void AnnotateAnalysisUi::DrawContent(AppController& app, bool* wantClose, const std::string& selectedJiraIssueKey) {
    ensureSettingsBuffersLoaded();

    MaybeAutoselectCallstackTrackerField(app.GetAvailableFields());
    MaybeAutoselectLastFoundClTrackerField(app.GetAvailableFields());
    MaybeAutoselectLastOccurrencesTrackerField(app.GetAvailableFields());

    const bool justOpened = !annotateOpenPrev_;
    annotateOpenPrev_ = true;
    if (justOpened) {
        // Re-seed the raw/table toggle from persisted config on each open (the close handler
        // resets the runtime flag to false; the persisted preference wins on reopen).
        State().showRaw = State().annotateCfg.ShowRawCallstack;
    }
    if (justOpened || selectedJiraIssueKey != State().lastCallstackIssueKey) {
        State().lastCallstackIssueKey = selectedJiraIssueKey;
        TryFillCallstackFromJira(app, selectedJiraIssueKey);
        TryFillBeforeChangelistAndDateFromJira(app, selectedJiraIssueKey);
    }

    if (State().annotatePendingAutoProcess && !State().worker.Running.load()) {
        RunAnnotateProcessFromBuffers();
        State().annotatePendingAutoProcess = false;
    }

    const AnnotateUiThemeColors& theme = State().annotateCfg.UiColors;

    std::vector<AnnotateRow> rowsSnap;
    size_t nrow = 0;
    {
        std::lock_guard<std::mutex> lk(State().displayMutex);
        rowsSnap = State().displayRows;
        nrow = rowsSnap.size();
    }

    const bool busy = State().worker.Running.load();
    const int prog = State().worker.Progress.load();

    AnnotateDrawCtx ctx{app, wantClose, selectedJiraIssueKey, theme, rowsSnap, nrow, busy, prog};

    if (DrawAnnotateHeaderToolbar(ctx)) {
        return;
    }
    ImGui::Separator();
    DrawAnnotateBannerAndStatus(ctx);

    if (ImGui::BeginTabBar("annotate_main_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Callstack")) {
            DrawAnnotateCallstackTab(ctx);
            ImGui::EndTabItem();
        }

        for (size_t ti = 0; ti < nrow; ++ti) {
            char tabName[80];
            std::snprintf(tabName, sizeof(tabName), "Entry %zu##AnnotateEntry%zu", ti + 1, ti);
            ImGuiTabItemFlags tflags = ImGuiTabItemFlags_None;
            if (State().pendingSelectEntryIndex == static_cast<int>(ti)) {
                tflags = ImGuiTabItemFlags_SetSelected;
            }
            if (ImGui::BeginTabItem(tabName, nullptr, tflags)) {
                if (State().pendingSelectEntryIndex == static_cast<int>(ti)) {
                    State().pendingSelectEntryIndex = -1;
                }
                DrawAnnotateEntryTab(ctx, ti);
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    DrawAnnotateAssignModal(ctx);
    DrawAnnotateProfileModal(ctx);
}
