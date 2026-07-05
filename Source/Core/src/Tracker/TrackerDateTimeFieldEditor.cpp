#include "TrackerDateTimeFieldEditor.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "TrackerDateTimePure.h"
#include "TicketFieldEditorCommitPolicyPure.h"
#include "Ui/TouchCellEditGesture.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

// SMATCHET_DEVIATION(rule=duplication; reason=UI-shell include boilerplate; owner=tracker-backend; revisit=2026-09-30)
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

using TrackerDateTimePure::ClampDayToMonth;
using TrackerDateTimePure::DaysInMonth;
using TrackerDateTimePure::DecMonth;
using TrackerDateTimePure::FirstOfMonthWeekday0Sun;
using TrackerDateTimePure::FormatFriendlyDate;
using TrackerDateTimePure::FormatFriendlyTime;
using TrackerDateTimePure::IncMonth;
using TrackerDateTimePure::ParseFriendlyDate;
using TrackerDateTimePure::ParseFriendlyTime;
using TrackerDateTimePure::TodayUtcParsed;

namespace {

// Compute the single-line display string for a date cell in its non-editing
// state. Falls back to the raw value, strips embedded newlines, and uses the
// caller-supplied format params (loading config only when they're empty).
std::string ComputeDateCellDisplay(const std::string& currentValue, const std::string& dateFormatOption,
                                   int thresholdDays) {
    std::string fmt = dateFormatOption;
    int thresh = thresholdDays;
    if (fmt.empty() || thresh <= 0) {
        const auto cfg = ConfigManager::Load();
        if (fmt.empty())
            fmt = cfg.DateFormatOption;
        if (thresh <= 0)
            thresh = cfg.DateCompactRelativeThresholdDays;
    }
    std::string display = FormatCompactJiraDateForDisplay(currentValue, fmt, thresh);
    if (display.empty()) {
        display = currentValue;
    }
    auto it = std::find_if(display.begin(), display.end(), [](char c) { return c == '\n' || c == '\r'; });
    if (it != display.end()) {
        display.erase(it, display.end());
    }
    if (display.empty()) {
        display = "";
    }
    return display;
}

static int InputTextCallback_ClearSelectOnEditOpen(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways || !data->UserData) {
        return 0;
    }
    auto* state = static_cast<SpreadsheetState*>(data->UserData);
    if (!state->PendingGridInputTextDeselect) {
        return 0;
    }
    const bool fullRange = data->BufTextLen > 0 && data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen;
    if (!data->EventActivated && !fullRange) {
        return 0;
    }
    const int end = data->BufTextLen;
    data->SetSelection(end, end);
    state->PendingGridInputTextDeselect = false;
    return 0;
}

const char* const kFullMonthNames[12] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};

enum class PickerAction { None, Apply, Clear, Cancel };

// Raw-ISO text-entry branch of the picker: an editable ISO string plus Apply/Cancel.
PickerAction DrawRawIsoEditor(ParsedJiraDateTime& working, char* textModeBuffer, size_t textModeBufferSize,
                              ImGuiInputTextCallback callback, void* callbackUserData) {
    ImGui::TextUnformatted("Edit raw ISO string:");
    ImGui::SetNextItemWidth(420.0f);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
    if (callback) {
        flags |= ImGuiInputTextFlags_CallbackAlways;
    }
    const bool submitted =
        ImGui::InputText("##rawiso", textModeBuffer, textModeBufferSize, flags, callback, callbackUserData);
    ParsedJiraDateTime tmp;
    const bool canParse = TryParseJiraDateTime(textModeBuffer, tmp);
    if (!canParse) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Apply parsed value") || (submitted && canParse)) {
        working = tmp;
        return PickerAction::Apply;
    }
    if (!canParse) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        return PickerAction::Cancel;
    }
    return PickerAction::None;
}

// Original/Selected value summary shown above the grid in full (non-dropdown) mode.
void DrawSelectionSummary(const ParsedJiraDateTime& working, bool isDateOnly, const std::string& originalValue) {
    std::string initialText =
        originalValue.empty() ? "" : FormatCompactJiraDateForDisplay(originalValue, "absolute_friendly");
    if (initialText.empty() && !originalValue.empty()) {
        initialText = originalValue;
    }

    std::string workingText;
    if (working.Year > 0) {
        if (isDateOnly) {
            char temp[64];
            std::snprintf(temp, sizeof(temp), "%04d-%02d-%02d", working.Year, working.Month, working.Day);
            workingText = temp;
        } else {
            char temp[128];
            std::snprintf(temp, sizeof(temp), "%04d-%02d-%02d, %02d:%02d:%02d UTC", working.Year, working.Month,
                          working.Day, working.Hour, working.Minute, working.Second);
            workingText = temp;
        }
    } else {
        workingText = "";
    }

    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Original: ");
    ImGui::SameLine();
    ImGui::TextUnformatted(initialText.c_str());

    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "Selected: ");
    ImGui::SameLine();
    ImGui::TextUnformatted(workingText.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

// Month-navigation header: the prev-year/prev-month chevrons, centered title, and
// next-month/next-year chevrons.
void DrawMonthNavHeader(int& viewYear, int& viewMonth) {
    char title[64];
    if (viewMonth >= 1 && viewMonth <= 12) {
        std::snprintf(title, sizeof(title), "%s %d", kFullMonthNames[viewMonth - 1], viewYear);
    } else {
        std::snprintf(title, sizeof(title), "%d-%02d", viewYear, viewMonth);
    }

    const float calendarWidth = 220.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));

    if (ImGui::Button("<<")) {
        viewYear--;
    }
    ImGui::SameLine();
    if (ImGui::Button("<")) {
        DecMonth(viewYear, viewMonth);
    }

    float titleWidth = ImGui::CalcTextSize(title).x;
    float titlePos = (calendarWidth - titleWidth) * 0.5f;
    ImGui::SameLine(titlePos);
    ImGui::TextUnformatted(title);

    float rightChevronsWidth = ImGui::CalcTextSize(">").x + ImGui::CalcTextSize(">>").x + 4.0f; // 4px spacing
    float rightPos = calendarWidth - rightChevronsWidth;
    ImGui::SameLine(rightPos);
    if (ImGui::Button(">")) {
        IncMonth(viewYear, viewMonth);
    }
    ImGui::SameLine();
    if (ImGui::Button(">>")) {
        viewYear++;
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
}

// Render a single calendar cell, owning its own PushID/PopID and style-colour pushes.
// Yields PickerAction::Apply and sets working when a day is clicked in dropdown mode; else None.
PickerAction DrawDayCell(ParsedJiraDateTime& working, int& viewYear, int& viewMonth, bool isDropdown, int dayCounter,
                         int dim, int prevDim, int idSeed) {
    ImGui::PushID(idSeed);
    if (dayCounter < 1) {
        const int d = prevDim + dayCounter;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        if (ImGui::SmallButton(std::to_string(d).c_str())) {
            DecMonth(viewYear, viewMonth);
            working.Year = viewYear;
            working.Month = viewMonth;
            working.Day = d;
            if (isDropdown) {
                ImGui::PopStyleColor();
                ImGui::PopID();
                return PickerAction::Apply;
            }
        }
        ImGui::PopStyleColor();
    } else if (dayCounter > dim) {
        const int d = dayCounter - dim;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        if (ImGui::SmallButton(std::to_string(d).c_str())) {
            IncMonth(viewYear, viewMonth);
            working.Year = viewYear;
            working.Month = viewMonth;
            working.Day = d;
            if (isDropdown) {
                ImGui::PopStyleColor();
                ImGui::PopID();
                return PickerAction::Apply;
            }
        }
        ImGui::PopStyleColor();
    } else {
        const int d = dayCounter;
        const bool selected = (working.Year == viewYear && working.Month == viewMonth && working.Day == d);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(31.0f / 255.0f, 116.0f / 255.0f, 236.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(51.0f / 255.0f, 136.0f / 255.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(21.0f / 255.0f, 96.0f / 255.0f, 216.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        if (ImGui::SmallButton(std::to_string(d).c_str())) {
            working.Year = viewYear;
            working.Month = viewMonth;
            working.Day = d;
            if (isDropdown) {
                if (selected)
                    ImGui::PopStyleColor(4);
                ImGui::PopID();
                return PickerAction::Apply;
            }
        }
        if (selected) {
            ImGui::PopStyleColor(4);
        }
    }
    ImGui::PopID();
    return PickerAction::None;
}

// The day-of-week header row plus the 6x7 day grid (prev/current/next month spill).
// Yields PickerAction::Apply and sets working when a day is picked in dropdown mode; else None.
PickerAction DrawDayGrid(ParsedJiraDateTime& working, int& viewYear, int& viewMonth, bool isDropdown) {
    const int wday0Sun = FirstOfMonthWeekday0Sun(viewYear, viewMonth);
    const int startOffset = wday0Sun;
    const int dim = DaysInMonth(viewYear, viewMonth);

    int prevYear = viewYear;
    int prevMonth = viewMonth;
    DecMonth(prevYear, prevMonth);
    const int prevDim = DaysInMonth(prevYear, prevMonth);

    const char* dow[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int c = 0; c < 7; ++c) {
        if (c > 0) {
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%s", dow[c]);
    }

    int dayCounter = 1 - startOffset;
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 7; ++col) {
            if (col > 0) {
                ImGui::SameLine();
            }
            const PickerAction cellAction =
                DrawDayCell(working, viewYear, viewMonth, isDropdown, dayCounter, dim, prevDim, row * 10 + col);
            if (cellAction != PickerAction::None) {
                return cellAction;
            }
            ++dayCounter;
        }
    }
    return PickerAction::None;
}

// Time-of-day drag fields (date-time only) and the Apply/Clear/Cancel/Raw-ISO action
// row, shown in full (non-dropdown) mode.
PickerAction DrawTimeAndActionFooter(ParsedJiraDateTime& working, bool isDateOnly, char* textModeBuffer,
                                     size_t textModeBufferSize, bool& forceTextMode) {
    if (!isDateOnly) {
        ImGui::Separator();
        int h = working.Hour;
        int m = working.Minute;
        int sec = working.Second;
        ImGui::TextUnformatted("Time (UTC wall)");
        ImGui::SetNextItemWidth(60.0f);
        ImGui::DragInt("##h", &h, 0.25f, 0, 23, "%02d h");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        ImGui::DragInt("##m", &m, 0.25f, 0, 59, "%02d m");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        ImGui::DragInt("##s", &sec, 0.25f, 0, 59, "%02d s");
        working.HasWallTime = true;
        working.Hour = (std::max)(0, (std::min)(23, h));
        working.Minute = (std::max)(0, (std::min)(59, m));
        working.Second = (std::max)(0, (std::min)(59, sec));
    } else {
        working.HasWallTime = false;
        working.Hour = 0;
        working.Minute = 0;
        working.Second = 0;
    }

    ImGui::Separator();
    if (ImGui::Button("Apply")) {
        return PickerAction::Apply;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        return PickerAction::Clear;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        return PickerAction::Cancel;
    }
    ImGui::SameLine();
    if (ImGui::Button("Raw ISO…")) {
        const std::string preview = FormatJiraDateOrDateTimeForApi(isDateOnly, working);
        std::snprintf(textModeBuffer, textModeBufferSize, "%s", preview.c_str());
        textModeBuffer[textModeBufferSize - 1] = '\0';
        forceTextMode = true;
    }
    return PickerAction::None;
}

PickerAction DrawCalendarPicker(ParsedJiraDateTime& working, int& viewYear, int& viewMonth, bool& forceTextMode,
                                bool isDateOnly, const std::string& originalValue, char* textModeBuffer,
                                size_t textModeBufferSize, ImGuiInputTextCallback callback = nullptr,
                                void* callbackUserData = nullptr, bool isDropdown = false) {
    if (forceTextMode) {
        return DrawRawIsoEditor(working, textModeBuffer, textModeBufferSize, callback, callbackUserData);
    }

    if (!isDropdown) {
        DrawSelectionSummary(working, isDateOnly, originalValue);
    }

    DrawMonthNavHeader(viewYear, viewMonth);

    const PickerAction gridAction = DrawDayGrid(working, viewYear, viewMonth, isDropdown);
    if (gridAction != PickerAction::None) {
        return gridAction;
    }

    if (!isDropdown) {
        return DrawTimeAndActionFooter(working, isDateOnly, textModeBuffer, textModeBufferSize, forceTextMode);
    }
    return PickerAction::None;
}

} // namespace

namespace TrackerDateTimeFieldEditor {

bool IsTrackerDateTimePickerField(const TrackerField& field) {
    if (field.ReadOnly) {
        return false;
    }
    return field.Type == "date" || field.Type == "datetime";
}

void RenderDateTimeFieldEditor(const CachedTicket& ticket, const TrackerField& field, const std::string& currentValue,
                               SpreadsheetState& state, const QueueDateTimeEditFn& queueEdit,
                               const std::string& dateFormatOption, int thresholdDays, bool singleClickToEdit) {
    const std::string editorKey = ticket.id + "::" + field.Id;
    const std::string itemId = "##DateCell_" + ticket.id + "_" + field.Id;
    const bool isDateOnly = (field.Type == "date");

    ImGui::PushID(editorKey.c_str());

    const auto queue = [&](const std::vector<std::string>& values) { queueEdit(ticket.id, field, values); };

    static ParsedJiraDateTime s_working{};
    static int s_viewYear = 2000;
    static int s_viewMonth = 1;
    static bool s_forceTextMode = false;

    if (!state.IsEditingField(ticket.id, field.Id)) {
        // Use caller-supplied format params when available; only Load() when called from non-hot paths.
        std::string display = ComputeDateCellDisplay(currentValue, dateFormatOption, thresholdDays);
        const bool blankValue = std::all_of(currentValue.begin(), currentValue.end(),
                                            [](unsigned char ch) { return std::isspace(ch) != 0; });
        // Empty due date (and similar) would otherwise require double-click like populated cells, so
        // a blank cell folds into the single-click open affordance. On the touch build this collapses
        // to a long-press (Ui/TouchCellEditGesture.h); desktop codegen stays byte-identical.
        const bool dtCellClicked =
            ImGui::Selectable((display + itemId).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (SmatchetTouchEdit::ShouldOpenCellEditorOnGesture(dtCellClicked, singleClickToEdit || blankValue)) {
            state.StartEditingField(ticket.id, field.Id, currentValue);
        }
        ImGui::PopID();
        return;
    }

    const bool editJustStarted = state.EditJustStarted;
    if (editJustStarted) {
        TrackerDateTimePure::InitDatePickerWorking(currentValue, isDateOnly, s_working, s_viewYear, s_viewMonth,
                                                   s_forceTextMode);
        if (SmatchetTouchEdit::kMobileTouchBuild) {
            // Touch: anchoring the picker at the touch point pins it to a screen edge and can clip the
            // Apply / Clear / Cancel footer off-screen (phone trap). Center it on the display instead.
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        } else {
            ImGui::SetNextWindowPos(ImGui::GetIO().MousePos, ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
        }
    }
    if (!ImGui::IsPopupOpen("picker")) {
        ImGui::OpenPopup("picker");
    }

    const bool modalOpen = ImGui::BeginPopupModal("picker", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (modalOpen) {
        // Back / Escape dismisses with no PUT (mirrors the inline mobile rule: focus-loss never
        // commits). A modal has no tap-away path, so dismissedByTapAway is always false here.
        const bool backPressed = ImGui::IsKeyPressed(ImGuiKey_Escape);

        PickerAction action = DrawCalendarPicker(s_working, s_viewYear, s_viewMonth, s_forceTextMode, isDateOnly,
                                                 currentValue, state.EditBuffer, sizeof(state.EditBuffer),
                                                 InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&state));

        const bool applyPressed = (action == PickerAction::Apply);
        const bool clearPressed = (action == PickerAction::Clear);
        const bool cancelPressed = (action == PickerAction::Cancel);

        // Apply and Clear are the explicit "Save" of this touch popup; Cancel / Back discard. The
        // commit gate (PUT only on a REAL change — canonical-form comparison so wire-format-only
        // differences never fire a stray no-op PUT) lives in TrackerDateTimePure::PlanDateTimeCommit
        // (lifted byte-identical; tests/Core/TrackerDateTimePure.test.cpp pins it).
        if (applyPressed || clearPressed) {
            const TrackerDateTimePure::DateTimeCommitPlan plan = TrackerDateTimePure::PlanDateTimeCommit(
                applyPressed, clearPressed, isDateOnly, s_working, currentValue);
            if (plan.Queue) {
                queue(plan.Values);
            }
        }

        if (TicketFieldEditorCommitPolicyPure::ShouldCloseTouchPopupEdit(
                /*savePressed=*/applyPressed || clearPressed, cancelPressed,
                /*dismissedByTapAway=*/false, backPressed)) {
            state.ClearEditing();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    } else {
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }

    if (editJustStarted) {
        state.EditJustStarted = false;
    }

    ImGui::PopID();
}

// Draw the calendar dropdown popup for the generic date picker. Owns the
// "calendar_dropdown" BeginPopup/EndPopup pair. On Apply, writes ioValue and
// sets valueChanged; retains the existing time-of-day from `parsed`.
void DrawGenericCalendarDropdown(const ParsedJiraDateTime& parsed, bool isDateTime, std::string& ioValue,
                                 bool& valueChanged) {
    static bool s_initWorking = false;

    if (!ImGui::IsPopupOpen("calendar_dropdown")) {
        s_initWorking = false;
    }

    ImGui::SetNextWindowSize(ImVec2(236.0f, 0.0f));
    if (ImGui::BeginPopup("calendar_dropdown")) {
        static ParsedJiraDateTime s_genWorking{};
        static int s_genViewYear = 2000;
        static int s_genViewMonth = 1;
        static bool s_genForceTextMode = false;
        if (!s_initWorking) {
            s_genWorking = parsed;
            s_genViewYear = parsed.Year;
            s_genViewMonth = parsed.Month;
            s_genForceTextMode = false;
            s_initWorking = true;
        }

        static char genRawBuf[128] = "";

        PickerAction action =
            DrawCalendarPicker(s_genWorking, s_genViewYear, s_genViewMonth, s_genForceTextMode, !isDateTime, ioValue,
                               genRawBuf, sizeof(genRawBuf), nullptr, nullptr, true);

        if (action == PickerAction::Apply) {
            ClampDayToMonth(s_genWorking, s_genWorking.Year, s_genWorking.Month);
            // Retain existing time
            s_genWorking.Hour = parsed.Hour;
            s_genWorking.Minute = parsed.Minute;
            s_genWorking.Second = parsed.Second;
            s_genWorking.HasWallTime = parsed.HasWallTime;

            ioValue = FormatJiraDateOrDateTimeForApi(!isDateTime, s_genWorking);
            valueChanged = true;
            s_initWorking = false;
            ImGui::CloseCurrentPopup();
        } else if (action == PickerAction::Cancel) {
            s_initWorking = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// Draw the time column (input + clear button + time_dropdown popup) for the
// generic date picker. Owns the "time_dropdown" BeginPopup/EndPopup pair.
// Mutates `parsed` / `ioValue` / `valueChanged` on edit.
void DrawGenericTimeColumn(float colWidth, ParsedJiraDateTime& parsed, bool isDateTime, std::string& ioValue,
                           bool& valueChanged, char* timeBuf, std::size_t timeBufSize) {
    ImGui::SameLine(0.0f, 6.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::SetNextItemWidth(colWidth - 26.0f);
    if (ImGui::InputText("##FriendlyTime", timeBuf, timeBufSize)) {
        int h = 0, m = 0;
        if (ParseFriendlyTime(timeBuf, h, m)) {
            parsed.Hour = h;
            parsed.Minute = m;
            parsed.Second = 0;
            parsed.HasWallTime = true;
            ioValue = FormatJiraDateOrDateTimeForApi(!isDateTime, parsed);
            valueChanged = true;
        }
    }
    if (ImGui::IsItemClicked()) {
        ImGui::OpenPopup("time_dropdown");
    }
    ImGui::SameLine();
    if (ImGui::Button("✖", ImVec2(24.0f, 0.0f))) {
        parsed.Hour = 0;
        parsed.Minute = 0;
        parsed.Second = 0;
        parsed.HasWallTime = true;
        ioValue = FormatJiraDateOrDateTimeForApi(!isDateTime, parsed);
        valueChanged = true;
    }
    ImGui::PopStyleVar();

    // Scrollable dropdown popup for recommended times
    ImGui::SetNextWindowSize(ImVec2(colWidth, 200.0f));
    if (ImGui::BeginPopup("time_dropdown")) {
        for (int h = 0; h < 24; ++h) {
            for (int m : {0, 30}) {
                int displayH = h;
                const char* ampm = "AM";
                if (displayH >= 12) {
                    ampm = "PM";
                    if (displayH > 12)
                        displayH -= 12;
                }
                if (displayH == 0)
                    displayH = 12;

                char timeStr[16];
                std::snprintf(timeStr, sizeof(timeStr), "%d:%02d %s", displayH, m, ampm);

                const bool isSelected = (parsed.Hour == h && parsed.Minute == m);
                if (ImGui::Selectable(timeStr, isSelected)) {
                    parsed.Hour = h;
                    parsed.Minute = m;
                    parsed.Second = 0;
                    parsed.HasWallTime = true;
                    ioValue = FormatJiraDateOrDateTimeForApi(!isDateTime, parsed);
                    valueChanged = true;
                    ImGui::CloseCurrentPopup();
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::EndPopup();
    }
}

bool RenderGenericDatePicker(const char* label, std::string& ioValue, bool isDateTime, float totalWidth) {
    ImGui::PushID(label);

    // Parse underlying ISO-8601 string to a ParsedJiraDateTime
    ParsedJiraDateTime parsed;
    if (!TryParseJiraDateTime(ioValue, parsed)) {
        parsed = TodayUtcParsed(isDateTime);
    }

    // Format localized / friendly strings for buffers
    char dateBuf[32];
    std::string fDate = FormatFriendlyDate(parsed);
    std::strncpy(dateBuf, fDate.c_str(), sizeof(dateBuf) - 1);
    dateBuf[sizeof(dateBuf) - 1] = '\0';

    char timeBuf[16];
    std::string fTime = FormatFriendlyTime(parsed);
    std::strncpy(timeBuf, fTime.c_str(), sizeof(timeBuf) - 1);
    timeBuf[sizeof(timeBuf) - 1] = '\0';

    bool valueChanged = false;

    if (totalWidth < 120.0f) {
        totalWidth = 120.0f;
    }
    const float colWidth = isDateTime ? ((totalWidth - 8.0f) * 0.5f) : totalWidth;

    // Column 1: Date Input Field with Calendar Icon
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::SetNextItemWidth(colWidth - 26.0f);
    if (ImGui::InputText("##FriendlyDate", dateBuf, sizeof(dateBuf))) {
        int y = 0, m = 0, d = 0;
        if (ParseFriendlyDate(dateBuf, y, m, d)) {
            parsed.Year = y;
            parsed.Month = m;
            parsed.Day = d;
            ioValue = FormatJiraDateOrDateTimeForApi(!isDateTime, parsed);
            valueChanged = true;
        }
    }
    if (ImGui::IsItemClicked()) {
        ImGui::OpenPopup("calendar_dropdown");
    }
    ImGui::SameLine();
    if (ImGui::Button("📅", ImVec2(24.0f, 0.0f))) {
        ImGui::OpenPopup("calendar_dropdown");
    }
    ImGui::PopStyleVar();

    // Dropdown Calendar Popup
    DrawGenericCalendarDropdown(parsed, isDateTime, ioValue, valueChanged);

    // Column 2 (Time Field): Side-by-side with close button, only if isDateTime is true
    if (isDateTime) {
        DrawGenericTimeColumn(colWidth, parsed, isDateTime, ioValue, valueChanged, timeBuf, sizeof(timeBuf));
    }

    ImGui::PopID();
    return valueChanged;
}

} // namespace TrackerDateTimeFieldEditor
