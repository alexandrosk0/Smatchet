#include "BlameAnalysisUi_Internal.h"

#include "AppController.h"
#include "CppSyntaxHighlight.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "P4Blame.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <vector>

using namespace BlameInternal;

void BlameAnalysisUi::DrawWindow(AppController& app, bool* pOpen, const std::string& selectedJiraIssueKey) {
    if (!pOpen || !*pOpen) {
        if (pOpen)
            CloseBlameModal(pOpen);
        return;
    }
    constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;
    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Annotate###BlameAnalysisModal", pOpen, kPanelFlags)) {
        CloseBlameModal(pOpen);
        ImGui::End();
        return;
    }
    if (!*pOpen) {
        CloseBlameModal(pOpen);
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        CloseBlameModal(pOpen);
        ImGui::End();
        return;
    }
    bool wantClose = false;
    DrawContent(app, &wantClose, selectedJiraIssueKey);
    if (wantClose)
        CloseBlameModal(pOpen);
    ImGui::End();
}

void BlameAnalysisUi::DrawContent(AppController& app, bool* wantClose, const std::string& selectedJiraIssueKey) {
    ensureSettingsBuffersLoaded();

    MaybeAutoselectCallstackTrackerField(app);
    MaybeAutoselectLastFoundClTrackerField(app);
    MaybeAutoselectLastOccurrencesTrackerField(app);

    const bool justOpened = !blameOpenPrev_;
    blameOpenPrev_ = true;
    if (justOpened || selectedJiraIssueKey != State().lastCallstackIssueKey) {
        State().lastCallstackIssueKey = selectedJiraIssueKey;
        TryFillCallstackFromJira(app, selectedJiraIssueKey);
        TryFillBeforeChangelistAndDateFromJira(app, selectedJiraIssueKey);
    }

    if (State().blamePendingAutoProcess && !State().worker.Running.load()) {
        RunBlameProcessFromBuffers();
        State().blamePendingAutoProcess = false;
    }

    const BlameUiThemeColors& theme = State().blameCfg.UiColors;

    const std::string titleIssue =
        selectedJiraIssueKey.empty() ? std::string("(no issue selected)") : selectedJiraIssueKey;
    ImGui::Text("Annotate: %s", titleIssue.c_str());
    ImGui::SameLine();
    {
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
        PushBlameLinkButtonColors(theme);
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
            ImGui::SetClipboardText(BuildBlameExportJson().c_str());
            State().lastUiStatus = "Annotate JSON export copied to clipboard.";
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Copy structured annotate export JSON (entries + nearby lines) to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Export CSV")) {
            ImGui::SetClipboardText(BuildBlameExportCsv().c_str());
            State().lastUiStatus = "Annotate CSV export copied to clipboard.";
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Copy one-row-per-entry annotate export CSV to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            PopBlameLinkButtonColors();
            if (wantClose)
                *wantClose = true;
            return;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Close Annotate.");
        }
        PopBlameLinkButtonColors();
    }

    ImGui::Separator();

    {
        const TrackerConnectivityBannerForUi jiraBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
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
    }

    if (!State().lastUiStatus.empty()) {
        ImGui::TextWrapped("%s", State().lastUiStatus.c_str());
        ImGui::Separator();
    }

    std::vector<BlameRow> rowsSnap;
    size_t nrow = 0;
    {
        std::lock_guard<std::mutex> lk(State().displayMutex);
        rowsSnap = State().displayRows;
        nrow = rowsSnap.size();
    }

    const bool busy = State().worker.Running.load();
    const int prog = State().worker.Progress.load();

    if (ImGui::BeginTabBar("blame_main_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Callstack")) {
            const bool streamlinedHide = State().blameStreamlinedFromGrid && !State().showRaw;

            ImGui::Text("Callstack Frames: %zu", nrow);
            {
                const ImGuiStyle& stBtn = ImGui::GetStyle();
                const float padH = stBtn.FramePadding.x * 2.f;
                const float callstackViewBtnW =
                    (std::max)(ImGui::CalcTextSize("Show Raw Text").x + padH,
                               (std::max)(ImGui::CalcTextSize("Show Table").x + padH,
                                          ImGui::CalcTextSize("Show raw callstack…").x + padH));
                if (!streamlinedHide) {
                    ImGui::SameLine();
                    PushBlameLinkButtonColors(theme);
                    if (State().showRaw) {
                        if (ImGui::Button("Show Table", ImVec2(callstackViewBtnW, 0.f))) {
                            State().showRaw = false;
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip(
                                "Return to the callstack table. The multiline field above the table becomes "
                                "editable again (when this tab shows it).");
                        }
                    } else {
                        if (ImGui::Button("Show Raw Text", ImVec2(callstackViewBtnW, 0.f))) {
                            State().showRaw = true;
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip(
                                "Show the callstack text in a wide, horizontally scrollable box (read-only in this "
                                "view). Use Show Table to edit the buffer in the smaller field.");
                        }
                    }
                    PopBlameLinkButtonColors();
                } else {
                    ImGui::SameLine();
                    PushBlameLinkButtonColors(theme);
                    if (ImGui::Button("Show raw callstack…", ImVec2(callstackViewBtnW, 0.f))) {
                        State().showRaw = true;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip(
                            "Reveal the callstack text, raw/table toggle, before changelist, and Process controls "
                            "(compact layout is used when Annotate is opened from the grid until you click this).");
                    }
                    PopBlameLinkButtonColors();
                }
            }

            if (!State().showRaw && nrow > 0) {
                ImGui::TextDisabled(
                    "Table: left-click the # column to open that frame in an Entry tab; right-click # for a menu "
                    "(Copy as TSV).");
            }

            if (!streamlinedHide) {
                ImGui::Spacing();
                ImGui::TextDisabled(
                    "Max frames, ignore list, P4 tools, and Jira callstack source: Settings → Preferences → "
                    "Annotate.");
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Before changelist");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "Optional: run annotate as of this Perforce changelist (each path is passed as "
                        "`depot/path@CL`).\n\n"
                        "Digits only; leave empty for the current head on each path.\n\n"
                        "The date control on the right resolves a calendar day to the first submitted changelist on "
                        "that day (using `p4 changes -r -m 1 -s submitted` on a server-wide `//...@start,end` range) "
                        "and copies the result into this field.");
                }
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
                    ImGui::SetTooltip(
                        "Pick a calendar day, then confirm in the calendar popup. The first submitted changelist "
                        "on that day (server date window) is written into the field on the left.");
                }
                ImGui::SameLine();
                if (TrackerDateTimeFieldEditor::RenderGenericDatePicker("##blame_before_day", State().beforeDateIso,
                                                                        false, 228.f)) {
                    ParsedJiraDateTime parsed;
                    if (TryParseJiraDateTime(State().beforeDateIso, parsed)) {
                        std::string cl;
                        std::string err;
                        if (P4FirstSubmittedChangelistOnCalendarDay(State().blameCfg, parsed.Year, parsed.Month,
                                                                    parsed.Day, cl, err)) {
                            std::snprintf(State().atClBuf, sizeof(State().atClBuf), "%s", cl.c_str());
                            State().lastUiStatus = "Before changelist set to first submitted CL on that day: " + cl;
                        } else {
                            State().lastUiStatus = err.empty() ? "Could not resolve changelist for that date." : err;
                        }
                    } else {
                        State().lastUiStatus = "Invalid date from picker.";
                    }
                }

                PushBlameLinkButtonColors(theme);
                if (ImGui::Button("Process") && !busy) {
                    RunBlameProcessFromBuffers();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !busy) {
                    ImGui::SetTooltip(
                        "Parse the callstack buffer and fetch Perforce annotate for each frame (options under "
                        "Preferences → Annotate). If you opened Annotate from the grid with a callstack field, "
                        "this usually runs once automatically after the buffer fills.");
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") && busy) {
                    State().worker.Cancel = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && busy) {
                    ImGui::SetTooltip("Stop the in-progress Annotate worker.");
                }
                PopBlameLinkButtonColors();
            } else if (busy) {
                PushBlameLinkButtonColors(theme);
                if (ImGui::Button("Cancel") && busy) {
                    State().worker.Cancel = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("Stop the in-progress Annotate worker.");
                }
                PopBlameLinkButtonColors();
            }

            if (!streamlinedHide) {
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
                    const float rawFieldW =
                        std::min(std::max(rawMaxLineW + ImGui::GetStyle().FramePadding.x * 2.f, 120.f), cap);

                    ImGui::BeginChild("##rawcs_scroll", ImVec2(rawFieldW, 220.f), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    DrawColoredCppText(State().callstackBuf);
                    ImGui::EndChild();
                } else {
                    ImGui::InputTextMultiline("##callstackpaste", State().callstackBuf, sizeof(State().callstackBuf),
                                              ImVec2(-1.f, 120.f), 0);
                }
            }

            if (!State().showRaw && nrow > 0) {
                // Keep Process / callstack editor above a dedicated scroll region. A tall in-table
                // ScrollY made the whole tab body exceed the viewport so the window scroll bar hid
                // the controls at the top (Process looked "missing").
                const float tblScrollH = std::max(ImGui::GetContentRegionAvail().y - 4.f, 80.f);
                ImGui::BeginChild("##blame_callstack_tbl_scroll", ImVec2(0.f, tblScrollH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_None);
                const float locColW = 250.f;
                if (ImGui::BeginTable("blame_tbl", 6,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
                                      ImVec2(-1.f, 0.f))) {
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.f, 0);
                    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 0.f, 1);
                    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, locColW, 2);
                    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 120.f, 3);
                    ImGui::TableSetupColumn("CL", ImGuiTableColumnFlags_WidthFixed, 70.f, 4);
                    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 90.f, 5);
                    ImGui::TableHeadersRow();

                    const float rowH = ImGui::GetTextLineHeightWithSpacing();
                    for (size_t i = 0; i < nrow; ++i) {
                        const BlameRow& row = rowsSnap[i];
                        const bool pending = busy && static_cast<int>(i) >= prog;
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        char idxBuf[16];
                        std::snprintf(idxBuf, sizeof(idxBuf), "%u", static_cast<unsigned>(i + 1));
                        PushBlameLinkTextOnly(theme);
                        ImGui::SelectableRaw(idxBuf, false,
                                             ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                             ImVec2(0.f, rowH));
                        PopBlameLinkTextOnly();
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                            State().pendingSelectEntryIndex = static_cast<int>(i);
                            EnsureDetailLoading(i, State().blameCfg, std::string(State().atClBuf));
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
                            ImGui::OpenPopup("blame_cs_row_copy");
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("Left-click: open the Entry tab for this frame and load its annotation.\n"
                                              "Right-click: menu with Copy (this row as TSV).");
                        }
                        if (ImGui::BeginPopup("blame_cs_row_copy")) {
                            if (ImGui::MenuItem("Copy")) {
                                ImGui::SetClipboardText(BuildCallstackRowTsv(row, i).c_str());
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const char* fn = row.Parsed.Function.c_str();
                            const float colAvail = ImGui::GetContentRegionAvail().x;
                            const float lineH = ImGui::GetTextLineHeight();
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::SelectableRaw(fn, false, ImGuiSelectableFlags_AllowOverlap,
                                                     ImVec2(colAvail, lineH))) {
                                /* click handled below */
                            }
                            PopBlameLinkTextOnly();
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
                            const std::string shortPath =
                                ShortenPathForDisplay(row.PathForP4, std::max(32.f, locCellW - 40.f));
                            const std::string shortLoc = shortPath + ":" + std::to_string(row.Parsed.LineNumber);
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::SelectableRaw(shortLoc.c_str(), false, ImGuiSelectableFlags_AllowOverlap)) {
                                LaunchP4VcLike(State().blameCfg, State().timeTpl, State().changeTpl, true,
                                               row.PathForP4, row.Parsed.LineNumber, row.Blame.Changelist);
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("Left-click: open p4vc timelapse for this file and line.\n\n%s",
                                                  fullLoc.c_str());
                            }
                        }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const std::string userDisp =
                                pending ? std::string("...")
                                        : (row.Blame.User.empty() ? std::string("-") : row.Blame.User);
                            const bool userActionable =
                                !pending && !row.Blame.User.empty() && row.Blame.User != "..." && row.Blame.User != "-";
                            if (pending || row.Blame.User.empty() || row.Blame.User == "-") {
                                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            } else {
                                PushBlameLinkTextOnly(theme);
                            }
                            ImGui::SelectableRaw((userDisp + "##user").c_str(), false,
                                                 ImGuiSelectableFlags_AllowOverlap);
                            if (pending || row.Blame.User.empty() || row.Blame.User == "-") {
                                ImGui::PopStyleColor();
                            } else {
                                PopBlameLinkTextOnly();
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                if (pending) {
                                    ImGui::SetTooltip("Waiting for Annotate results for this row.");
                                } else if (userActionable) {
                                    ImGui::SetTooltip("Left-click: look up this Perforce user in Jira.\n"
                                                      "Right-click: open the assign dialog for this row.\n%s",
                                                      row.Blame.Approximate
                                                          ? "\nApproximate Annotate (line may not match exact CL)."
                                                          : "");
                                } else {
                                    ImGui::SetTooltip("No Perforce user on this row.");
                                }
                            }
                            if (!pending && ImGui::IsItemClicked() && !row.Blame.User.empty() &&
                                row.Blame.User != "...") {
                                OpenTrackerUserProfileForP4User(app, row.Blame.User);
                            }
                            if (ImGui::IsMouseClicked(1) && ImGui::IsItemHovered()) {
                                PrepareAssignModal(app, row, pending ? std::string() : row.Blame.User);
                                ImGui::OpenPopup("blame_assign");
                            }
                        }

                        ImGui::TableSetColumnIndex(4);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const std::string clDisp =
                                pending ? std::string("...")
                                        : (row.Blame.Changelist.empty() ? std::string("-") : row.Blame.Changelist);
                            if (pending || row.Blame.Changelist.empty()) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            } else {
                                PushBlameLinkTextOnly(theme);
                            }
                            ImGui::SelectableRaw((clDisp + "##cl").c_str(), false, ImGuiSelectableFlags_AllowOverlap);
                            if (pending || row.Blame.Changelist.empty()) {
                                ImGui::PopStyleColor();
                            } else {
                                PopBlameLinkTextOnly();
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                if (pending) {
                                    ImGui::SetTooltip("Waiting for Annotate results for this row.");
                                } else if (row.Blame.Changelist.empty()) {
                                    ImGui::SetTooltip("No changelist on this row.");
                                } else {
                                    DrawClTooltipAsync(row.Blame.Changelist, State().blameCfg, theme);
                                }
                            }
                            if (ImGui::IsItemClicked() && !pending && !row.Blame.Changelist.empty()) {
                                LaunchP4VcLike(State().blameCfg, State().timeTpl, State().changeTpl, false,
                                               row.PathForP4, row.Parsed.LineNumber, row.Blame.Changelist);
                            }
                        }

                        ImGui::TableSetColumnIndex(5);
                        ImGui::SetNextItemAllowOverlap();
                        if (pending) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::TextUnformatted("-");
                            ImGui::PopStyleColor();
                        } else if (row.Blame.Date.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::TextUnformatted("-");
                            ImGui::PopStyleColor();
                        } else {
                            const std::string dd = NormalizeDateDisplay(row.Blame.Date);
                            ImGui::TextUnformatted(dd.c_str());
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
            }

            ImGui::EndTabItem();
        }

        for (size_t ti = 0; ti < nrow; ++ti) {
            char tabName[80];
            std::snprintf(tabName, sizeof(tabName), "Entry %zu##BlameEntry%zu", ti + 1, ti);
            ImGuiTabItemFlags tflags = ImGuiTabItemFlags_None;
            if (State().pendingSelectEntryIndex == static_cast<int>(ti)) {
                tflags = ImGuiTabItemFlags_SetSelected;
            }
            if (ImGui::BeginTabItem(tabName, nullptr, tflags)) {
                if (State().pendingSelectEntryIndex == static_cast<int>(ti)) {
                    State().pendingSelectEntryIndex = -1;
                }
                EnsureDetailLoading(ti, State().blameCfg, std::string(State().atClBuf));
                BlameRow row = rowsSnap[ti];
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
                                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                             ImVec2(0, ImGui::GetContentRegionAvail().y - 8.f))) {
                    ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 60.f);
                    ImGui::TableSetupColumn("CL", ImGuiTableColumnFlags_WidthFixed, 70.f);
                    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 120.f);
                    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 100.f);
                    ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    const int targetLine = row.Parsed.LineNumber;
                    const std::vector<P4AnnotatedLine>& lines = State().detailData[ti].Lines;
                    const ImU32 hlU32 = ImGui::ColorConvertFloat4ToU32(ThCol(theme.FindHighlight));
                    const float annRowHitH = ImGui::GetTextLineHeightWithSpacing();
                    auto annRowOpenCopyMenu = []() {
                        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
                            ImGui::OpenPopup("blame_ann_row_copy");
                        }
                    };
                    auto annRowHoverTip = []() {
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip(
                                "Right-click any cell in this row for a menu: Copy (full line as TSV). Assign... "
                                "appears only when the Perforce user can be used for assign (not '-' or '...').");
                        }
                    };
                    for (size_t lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
                        const P4AnnotatedLine& ln = lines[lineIdx];
                        ImGui::PushID(static_cast<int>(lineIdx));
                        ImGui::TableNextRow();
                        const bool hl = (ln.SourceLine == targetLine);
                        if (hl) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, hlU32);
                        }

                        ImGui::TableSetColumnIndex(0);
                        {
                            const float markW = ImGui::GetContentRegionAvail().x;
                            if (hl) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.StatusWarning));
                            }
                            PushBlameLinkTextOnly(theme);
                            ImGui::SelectableRaw(hl ? ">>>" : " ", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2((std::max)(markW, 1.f), annRowHitH));
                            PopBlameLinkTextOnly();
                            if (hl) {
                                ImGui::PopStyleColor();
                            }
                            annRowOpenCopyMenu();
                            annRowHoverTip();
                        }

                        ImGui::TableSetColumnIndex(1);
                        {
                            char lineBuf[32];
                            std::snprintf(lineBuf, sizeof(lineBuf), "%d", ln.SourceLine);
                            const float lineColW = ImGui::GetContentRegionAvail().x;
                            PushBlameLinkTextOnly(theme);
                            ImGui::SelectableRaw(lineBuf, false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2((std::max)(lineColW, 1.f), annRowHitH));
                            PopBlameLinkTextOnly();
                            annRowOpenCopyMenu();
                            annRowHoverTip();
                        }

                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemAllowOverlap();
                        const float clCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
                        if (ln.Changelist.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::SelectableRaw("-##cl", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(clCellW, annRowHitH));
                            ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("No changelist on this line. Right-click for row menu (Copy).");
                            }
                        } else {
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::SelectableRaw(ln.Changelist.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                                     ImVec2(clCellW, annRowHitH))) {
                                LaunchP4VcLike(State().blameCfg, State().timeTpl, State().changeTpl, false,
                                               row.PathForP4, ln.SourceLine, ln.Changelist);
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                DrawClTooltipAsync(ln.Changelist, State().blameCfg, theme);
                            }
                        }
                        annRowOpenCopyMenu();

                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemAllowOverlap();
                        const float userCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
                        if (ln.User.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::SelectableRaw("-##user", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(userCellW, annRowHitH));
                            ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                std::string tip = "No Perforce user on this line. Right-click for row menu (Copy).";
                                if (hl && row.Blame.Approximate) {
                                    tip += "\n\nApproximate row: unable to find exact CL and user.";
                                }
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                        } else {
                            PushBlameLinkTextOnly(theme);
                            ImGui::SelectableRaw(ln.User.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(userCellW, annRowHitH));
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                std::string tip = "Left-click: look up this Perforce user in Jira.\n"
                                                  "Right-click: row menu - Copy (full line as TSV); Assign... when the "
                                                  "user is eligible (not '-' or '...').";
                                if (hl && row.Blame.Approximate) {
                                    tip += "\n\nApproximate row: unable to find exact CL and user.";
                                }
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                                OpenTrackerUserProfileForP4User(app, ln.User);
                            }
                        }
                        annRowOpenCopyMenu();

                        ImGui::TableSetColumnIndex(4);
                        const float dateCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
                        if (ln.Date.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::SelectableRaw("-##date", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(dateCellW, annRowHitH));
                            ImGui::PopStyleColor();
                        } else {
                            const std::string dd = NormalizeDateDisplay(ln.Date);
                            ImGui::SelectableRaw(dd.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(dateCellW, annRowHitH));
                        }
                        annRowOpenCopyMenu();
                        annRowHoverTip();

                        ImGui::TableSetColumnIndex(5);
                        {
                            const float codeColW = ImGui::GetContentRegionAvail().x;
                            const ImVec2 codeCell0 = ImGui::GetCursorScreenPos();
                            DrawColoredCppLine(ln.Code.c_str());
                            ImGui::SetCursorScreenPos(codeCell0);
                            ImGui::SelectableRaw("##ann_code_rmb", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2((std::max)(codeColW, 1.f), annRowHitH));
                            annRowOpenCopyMenu();
                            annRowHoverTip();
                        }

                        if (ImGui::BeginPopup("blame_ann_row_copy")) {
                            if (ImGui::MenuItem("Copy")) {
                                ImGui::SetClipboardText(BuildAnnotatedRowTsv(ln).c_str());
                            }
                            const bool canAssign = !ln.User.empty() && ln.User != "-" && ln.User != "...";
                            if (canAssign) {
                                if (ImGui::MenuItem("Assign...")) {
                                    BlameRow br = row;
                                    br.Blame.User = ln.User;
                                    br.Blame.Changelist = ln.Changelist;
                                    br.Parsed.LineNumber = ln.SourceLine;
                                    br.Blame.LineSnippet = ln.Code;
                                    br.Blame.Date = ln.Date;
                                    br.Blame.Approximate = false;
                                    PrepareAssignModal(app, br, ln.User);
                                    ImGui::CloseCurrentPopup();
                                    ImGui::OpenPopup("blame_assign");
                                }
                            }
                            ImGui::EndPopup();
                        }

                        if (hl && ti < State().detailScrolled.size() && !State().detailScrolled[ti]) {
                            ImGui::SetScrollHereY(0.5f);
                            State().detailScrolled[ti] = true;
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    if (ImGui::BeginPopupModal("blame_assign", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
        if (selectedJiraIssueKey.empty()) {
            ImGui::TextDisabled("Select a Jira issue in the grid.");
        } else {
            const bool hasJiraAccount = State().assignHasJiraAccount && !State().assignAccountId.empty();
            PushBlameLinkTextOnly(theme);
            ImGui::BeginDisabled(readOnlyMode);
            if (ImGui::Selectable("Assign issue to user", false)) {
                std::string err;
                const TrackerField* f = app.FindFieldById("assignee");
                if (!hasJiraAccount) {
                    LOG_ERROR("Blame UI: assign skipped — no Jira account match for this Perforce user.");
                    State().lastUiStatus = "No Jira user match for assign.";
                } else if (!f) {
                    LOG_ERROR("Blame UI: assignee field not in catalog.");
                    State().lastUiStatus = "assignee field not in catalog.";
                } else if (app.SubmitFieldEdit(selectedJiraIssueKey, *f, {State().assignAccountId}, err)) {
                    LOG_INFO("Blame UI: assignee set on %s", selectedJiraIssueKey.c_str());
                    State().lastUiStatus = "Assignee updated.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: assign failed: %s", err.c_str());
                    State().lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            PopBlameLinkTextOnly();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "Set the Jira assignee on the selected issue to the Jira user matched from the Perforce user on "
                    "the Annotate row you used (callstack table or Entry tab row menu).\n"
                    "Requires a matching Jira account; otherwise an error is shown.");
            }
            ImGui::BeginDisabled(readOnlyMode);
            if (ImGui::Selectable("Add Annotate context comment", false)) {
                std::string err;
                if (app.AddIssueCommentBlameContext(
                        selectedJiraIssueKey, State().assignRow.Blame.User, State().assignRow.Parsed.Function,
                        State().assignRow.PathForP4, State().assignRow.Parsed.LineNumber,
                        State().assignRow.Blame.Changelist, State().assignRow.Blame.Date,
                        State().assignRow.Blame.Approximate, State().assignRow.Blame.LineSnippet, err)) {
                    LOG_INFO("Blame UI: posted blame context comment for %s.", selectedJiraIssueKey.c_str());
                    State().lastUiStatus = "Annotate context comment posted.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: comment failed: %s", err.c_str());
                    State().lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextDisabled("Quick comment templates");
            ImGui::BeginDisabled(readOnlyMode);
            {
                const TrackerConfig jiraCfg = cfg;
                int blameTplIndex = 0;
                for (const auto& t : jiraCfg.BlameCommentTemplates) {
                    if (!t.Id.empty()) {
                        ImGui::PushID(t.Id.c_str());
                    } else {
                        ImGui::PushID(blameTplIndex);
                    }
                    if (ImGui::SelectableRaw(t.Title.c_str(), false)) {
                        std::string err;
                        std::string commentBody = BuildBlameQuickCommentTemplate(
                            selectedJiraIssueKey, t.Id, State().assignRow, jiraCfg.BlameCommentTemplates);
                        if (app.AddIssueCommentPlain(selectedJiraIssueKey, commentBody, err)) {
                            State().lastUiStatus = "Posted '" + t.Title + "' comment.";
                            ImGui::CloseCurrentPopup();
                        } else {
                            State().lastUiStatus = err.empty() ? "Failed to post Jira comment." : err;
                        }
                    }
                    ImGui::PopID();
                    ++blameTplIndex;
                }
            }
            ImGui::EndDisabled();
            if (readOnlyMode && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Disabled while Jira is in read-only mode.");
            }
            if (hasJiraAccount) {
                PushBlameLinkTextOnly(theme);
            }
            ImGui::BeginDisabled(!hasJiraAccount || readOnlyMode);
            if (ImGui::Selectable("Assign and add Annotate context", false)) {
                std::string err;
                const TrackerField* f = app.FindFieldById("assignee");
                bool assigned = true;
                if (!f) {
                    err = "assignee field not in catalog.";
                    assigned = false;
                } else {
                    assigned = app.SubmitFieldEdit(selectedJiraIssueKey, *f, {State().assignAccountId}, err);
                }
                if (assigned &&
                    app.AddIssueCommentBlameContext(
                        selectedJiraIssueKey, State().assignRow.Blame.User, State().assignRow.Parsed.Function,
                        State().assignRow.PathForP4, State().assignRow.Parsed.LineNumber,
                        State().assignRow.Blame.Changelist, State().assignRow.Blame.Date,
                        State().assignRow.Blame.Approximate, State().assignRow.Blame.LineSnippet, err)) {
                    LOG_INFO("Blame UI: assigned %s and posted blame context comment.", selectedJiraIssueKey.c_str());
                    State().lastUiStatus = "Assigned and commented.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: assign/comment failed: %s", err.c_str());
                    State().lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            if (hasJiraAccount) {
                PopBlameLinkTextOnly();
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
        PushBlameLinkButtonColors(theme);
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Close this dialog without making further changes.");
        }
        PopBlameLinkButtonColors();
        ImGui::EndPopup();
    }

    if (State().openProfileModal) {
        ImGui::OpenPopup("Jira user profile");
        State().openProfileModal = false;
    }
    if (ImGui::BeginPopupModal("Jira user profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
        PushBlameLinkButtonColors(theme);
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Close the Jira user profile dialog.");
        }
        PopBlameLinkButtonColors();
        ImGui::EndPopup();
    }
}
