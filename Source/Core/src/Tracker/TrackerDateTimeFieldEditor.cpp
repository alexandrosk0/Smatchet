#include "TrackerDateTimeFieldEditor.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "TrackerDateTimePure.h"
#include "TicketFieldEditorCommitPolicyPure.h"
#include "TouchCellEditGesture.h"
#include "TrackerFieldValueParseExplain.h"
// SMATCHET_DEVIATION(rule=no-ui-include-in-domain; reason=this TU is an ImGui calendar/date widget that predates the zone rule and already draws UI; the inline error is the shared draw helper; owner=orchestrator; revisit=2027-02-16)
#include "Ui/SmatchetInlineError.h"
// SMATCHET_DEVIATION(rule=no-ui-include-in-domain; reason=a rejected typed date raises the same toast every other editor does; the widget has no other way to tell the user; owner=orchestrator; revisit=2027-02-16)
#include "Ui/SmatchetToast.h"
#include "imgui.h"
#include "imgui_internal.h" // SetKeyOwner / owner-aware IsKeyPressed for the calendar keys
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

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

// Today on the user's own calendar, and the tolerant reading of a typed date relative to it. Month first, like
// the "M/D/YYYY" the fields have always shown; a number over 12 still fixes the order.
TrackerParseExplain::CivilDate LocalToday() {
    TrackerParseExplain::CivilDate today;
    if (!TrackerDateTimePure::TodayLocalYmd(today.Year, today.Month, today.Day)) {
        const ParsedJiraDateTime utc = TodayUtcParsed(false);
        today.Year = utc.Year;
        today.Month = utc.Month;
        today.Day = utc.Day;
    }
    return today;
}

TrackerParseExplain::DateParse ParseTypedDate(const std::string& text) {
    return TrackerParseExplain::ParseDateInput(text, LocalToday(), TrackerParseExplain::DateOrder::MonthFirst);
}

// The "= 2026-11-20" preview under a date that reads in a spelling other than the field's own.
std::string DatePreviewText(const TrackerParseExplain::DateParse& parsed) {
    return SmatchetLocalization::Format("parse.date.preview", "= %s",
                                        TrackerParseExplain::FormatIsoDate(parsed.Date).c_str());
}

// The "Type a date" row of the grid's date picker (#40): the same tolerant reading as the generic field.
// Apply stays off while the text there does not read, so a half-typed date is never applied by accident.
struct TypedDateRow {
    char Buf[64] = {};
    bool BlocksApply = false;
};
TypedDateRow g_typedDateRow;

void ResetTypedDateRow() { g_typedDateRow = TypedDateRow(); }

// Draws the row. Returns true when Enter was pressed on a date that reads (the picker applies it).
bool DrawTypedDateRow(ParsedJiraDateTime& working, int& viewYear, int& viewMonth) {
    ImGui::TextUnformatted("Type a date:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##typedDate", SmatchetLocalization::T("date_field.type_hint", "tomorrow, fri, +3d, 2026-11-20"),
        g_typedDateRow.Buf, sizeof(g_typedDateRow.Buf), ImGuiInputTextFlags_EnterReturnsTrue);
    const bool edited = ImGui::IsItemEdited();
    const TrackerParseExplain::DateParse parsed = ParseTypedDate(g_typedDateRow.Buf);
    g_typedDateRow.BlocksApply = !parsed.Ok && !parsed.Empty;
    if (enter && g_typedDateRow.BlocksApply) {
        ImGui::SetKeyboardFocusHere(-1); // Enter on text that does not read: the caret stays in the row to fix it
    }
    if (parsed.Ok) {
        if (edited || enter) {
            working.Year = parsed.Date.Year;
            working.Month = parsed.Date.Month;
            working.Day = parsed.Date.Day;
            viewYear = working.Year;
            viewMonth = working.Month;
        }
        smatchet::ui::DrawInlineParseHint(DatePreviewText(parsed));
    } else if (g_typedDateRow.BlocksApply) {
        smatchet::ui::DrawInlineParseError(parsed.Message);
    }
    return enter && parsed.Ok;
}

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

// Set when a calendar key or quick pick moves the working day: the day grid puts the keyboard cursor on that
// day as it draws it, so arrows and Enter carry on from there. One calendar is open at a time (each lives in
// its own popup), so one flag serves both pickers. UI thread only.
bool s_focusWorkingDay = false;

// Puts `working` (and the month on show) on y-m-d. `focusDay` also asks the grid to put the keyboard cursor
// on that day; a pick that applies and closes the popup at once passes false, so nothing is left armed.
void MoveWorkingDay(ParsedJiraDateTime& working, int& viewYear, int& viewMonth, int y, int m, int d, bool focusDay) {
    working.Year = y;
    working.Month = m;
    working.Day = d;
    viewYear = y;
    viewMonth = m;
    s_focusWorkingDay = focusDay;
}

// The month on show with the working day's day-of-month clamped into it. The page and home / end keys step
// from here, not from the working day, so they carry on from a month the chevrons browsed to.
void ViewedDate(const ParsedJiraDateTime& working, int viewYear, int viewMonth, int& y, int& m, int& d) {
    y = viewYear;
    m = (viewMonth >= 1 && viewMonth <= 12) ? viewMonth : 1;
    d = (std::max)(1, (std::min)(working.Day >= 1 ? working.Day : 1, DaysInMonth(y, m)));
}

// The calendar's own keys, read while its popup has the keyboard and nothing is being typed: PageUp /
// PageDown a month, with Shift a year; Home / End the first / last day; T today; Ctrl+Enter applies in the
// full picker. The arrows stay ImGui's own navigation between the day buttons, and Enter picks the focused
// day. Returns true for Ctrl+Enter.
bool HandleCalendarKeys(ParsedJiraDateTime& working, int& viewYear, int& viewMonth, bool isDropdown) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) {
        return false;
    }
    const ImGuiID owner = ImGui::GetID("##calendar_keys");
    // Claimed every frame while focused, so ImGui's own page and home navigation does not also move the
    // keyboard cursor to the top or bottom of the popup on the same press.
    ImGui::SetKeyOwner(ImGuiKey_PageUp, owner);
    ImGui::SetKeyOwner(ImGuiKey_PageDown, owner);
    ImGui::SetKeyOwner(ImGuiKey_Home, owner);
    ImGui::SetKeyOwner(ImGuiKey_End, owner);

    int y = 0;
    int m = 0;
    int d = 0;
    ViewedDate(working, viewYear, viewMonth, y, m, d);
    const bool plain = !io.KeyCtrl && !io.KeyAlt && !io.KeySuper;
    const int monthStep = io.KeyShift ? 12 : 1;
    if (plain && ImGui::IsKeyPressed(ImGuiKey_PageUp, ImGuiInputFlags_Repeat, owner)) {
        TrackerDateTimePure::AddMonthsClampDay(y, m, d, -monthStep);
        MoveWorkingDay(working, viewYear, viewMonth, y, m, d, true);
    } else if (plain && ImGui::IsKeyPressed(ImGuiKey_PageDown, ImGuiInputFlags_Repeat, owner)) {
        TrackerDateTimePure::AddMonthsClampDay(y, m, d, monthStep);
        MoveWorkingDay(working, viewYear, viewMonth, y, m, d, true);
    } else if (plain && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Home, ImGuiInputFlags_None, owner)) {
        MoveWorkingDay(working, viewYear, viewMonth, y, m, 1, true);
    } else if (plain && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_End, ImGuiInputFlags_None, owner)) {
        MoveWorkingDay(working, viewYear, viewMonth, y, m, DaysInMonth(y, m), true);
    } else if (plain && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        if (TrackerDateTimePure::TodayLocalDate(y, m, d)) {
            MoveWorkingDay(working, viewYear, viewMonth, y, m, d, true);
        }
    }
    const bool ctrlEnter =
        io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    return !isDropdown && ctrlEnter;
}

// Today / Tomorrow / In a week. In the dropdown a quick pick applies at once, as a day click does; in the
// full picker it only moves the working day, which Apply then saves.
PickerAction DrawQuickPicks(ParsedJiraDateTime& working, int& viewYear, int& viewMonth, bool isDropdown) {
    struct QuickPick {
        const char* key;
        const char* fallback;
        int days;
    };
    static const QuickPick kPicks[] = {
        {"calendar.today", "Today", 0},
        {"calendar.tomorrow", "Tomorrow", 1},
        {"calendar.in_a_week", "In a week", 7},
    };
    PickerAction action = PickerAction::None;
    for (std::size_t i = 0; i < sizeof(kPicks) / sizeof(kPicks[0]); ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        // The label is already translated: the wrapper's English-source lookup would miss it.
        if (::ImGui::SmallButton(SmatchetLocalization::T(kPicks[i].key, kPicks[i].fallback))) {
            int y = 0;
            int m = 0;
            int d = 0;
            if (TrackerDateTimePure::TodayLocalDate(y, m, d)) {
                TrackerDateTimePure::AddDaysToDate(y, m, d, kPicks[i].days);
                MoveWorkingDay(working, viewYear, viewMonth, y, m, d, /*focusDay=*/false); // a click: no keyboard ring
                if (isDropdown) {
                    action = PickerAction::Apply;
                }
            }
        }
        if (i == 0) {
            ImGui::SetItemTooltip("%s", SmatchetLocalization::T("calendar.keys_hint",
                                                                "T today, PageUp / PageDown a month, Shift a year, "
                                                                "Home / End the month's first / last day"));
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    return action;
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
        const bool clicked = ImGui::SmallButton(std::to_string(d).c_str());
        if (selected) {
            // The popup opens with the keyboard on the chosen day, and a calendar key that moved the day
            // brings the keyboard cursor along.
            ImGui::SetItemDefaultFocus();
            if (s_focusWorkingDay) {
                s_focusWorkingDay = false;
                ImGui::FocusItem();
                ImGui::SetNavCursorVisible(true);
            }
        }
        if (clicked) {
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
    const bool applyBlocked = g_typedDateRow.BlocksApply;
    if (applyBlocked) {
        ImGui::BeginDisabled();
    }
    const bool applyClicked = ImGui::Button("Apply");
    if (applyBlocked) {
        ImGui::EndDisabled();
    }
    if (applyClicked) {
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
    s_focusWorkingDay = false; // armed and consumed within one call; nothing carries over from an earlier frame
    if (forceTextMode) {
        return DrawRawIsoEditor(working, textModeBuffer, textModeBufferSize, callback, callbackUserData);
    }

    if (!isDropdown) {
        DrawSelectionSummary(working, isDateOnly, originalValue);
        if (DrawTypedDateRow(working, viewYear, viewMonth)) {
            return PickerAction::Apply;
        }
    }

    const bool applyByKey = HandleCalendarKeys(working, viewYear, viewMonth, isDropdown);
    const PickerAction quickAction = DrawQuickPicks(working, viewYear, viewMonth, isDropdown);
    if (quickAction != PickerAction::None) {
        return quickAction;
    }

    DrawMonthNavHeader(viewYear, viewMonth);

    const PickerAction gridAction = DrawDayGrid(working, viewYear, viewMonth, isDropdown);
    s_focusWorkingDay = false;
    if (gridAction != PickerAction::None) {
        return gridAction;
    }

    if (!isDropdown) {
        const PickerAction footer =
            DrawTimeAndActionFooter(working, isDateOnly, textModeBuffer, textModeBufferSize, forceTextMode);
        return applyByKey && footer == PickerAction::None ? PickerAction::Apply : footer;
    }
    return PickerAction::None;
}

} // namespace

namespace {

// What the user has typed into a generic date field, kept from one frame to the next so the text survives
// until it is committed (Enter, or the caret leaving the field). One field has the caret at a time.
struct TypedDateState {
    ImGuiID Id = 0;
    std::string Text;
    bool Edited = false;   ///< the text was typed into since the caret arrived
    int RefocusFrames = 0; ///< frames left to put the caret back after an Enter that did not read
};
TypedDateState g_typedDate;

constexpr int kRefocusFrames = 3;
constexpr std::size_t kTypedDateBufSize = 64;

// Draws the feedback for a date being typed and says what to do with it. Returns true when `outDate` is a
// date to commit now.
bool ResolveTypedDate(ImGuiID dateId, bool enterPressed, bool active, bool deactivated, const ImVec2& rectMin,
                      const ImVec2& rectMax, TrackerParseExplain::CivilDate& outDate) {
    TypedDateState& typed = g_typedDate;
    if (typed.Id != dateId || !typed.Edited) {
        return false;
    }
    const TrackerParseExplain::DateParse parsed = ParseTypedDate(typed.Text);
    const bool unreadable = !parsed.Ok && !parsed.Empty;
    const bool refocusing = typed.RefocusFrames > 0;
    if (active || refocusing) {
        if (unreadable) {
            smatchet::ui::DrawInlineParseErrorBelowRect(parsed.Message, rectMin, rectMax);
        } else if (parsed.Ok) {
            smatchet::ui::DrawInlineParseHintBelowRect(DatePreviewText(parsed), rectMin, rectMax);
        }
    }
    if (active) {
        typed.RefocusFrames = 0; // the caret is back
        return false;
    }
    if (enterPressed) {
        if (parsed.Ok) {
            outDate = parsed.Date;
            typed = TypedDateState();
            return true;
        }
        // Enter on text that does not read: keep what was typed and put the caret back, error showing.
        typed.RefocusFrames = unreadable ? kRefocusFrames : 0;
        if (!unreadable) {
            typed = TypedDateState();
        }
        return false;
    }
    if (deactivated) {
        const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        typed = TypedDateState();
        if (escape || parsed.Empty) {
            return false; // Esc leaves the field as it was, without a word
        }
        if (parsed.Ok) {
            outDate = parsed.Date;
            return true;
        }
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("date_field.not_changed", "Date not changed"),
                                              TrackerParseExplain::DescribeParseMessage(parsed.Message),
                                              ToastType::Warning);
        return false;
    }
    if (refocusing && --typed.RefocusFrames == 0) {
        typed = TypedDateState(); // the caret never came back: let the field show its date again
    }
    return false;
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
        // to a long-press (TouchCellEditGesture.h); desktop codegen stays byte-identical.
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
        ResetTypedDateRow();
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
    char dateBuf[kTypedDateBufSize];
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

    // Column 1: Date Input Field with Calendar Icon. The field keeps what is typed in it until Enter or the
    // caret leaving commits it; "tomorrow", "fri", "+3d", "2026-11-20" and "11/20" all read (#40).
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    const ImGuiID dateId = ImGui::GetID("##FriendlyDate");
    if (g_typedDate.Id == dateId && g_typedDate.Edited &&
        (g_typedDate.RefocusFrames > 0 || ImGui::GetActiveID() == dateId)) {
        std::snprintf(dateBuf, sizeof(dateBuf), "%s", g_typedDate.Text.c_str());
        if (g_typedDate.RefocusFrames > 0) {
            ImGui::SetKeyboardFocusHere();
        }
    }
    ImGui::SetNextItemWidth(colWidth - 26.0f);
    const bool enterPressed =
        ImGui::InputText("##FriendlyDate", dateBuf, kTypedDateBufSize, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool dateEdited = ImGui::IsItemEdited();
    const bool dateActive = ImGui::IsItemActive();
    const bool dateDeactivated = ImGui::IsItemDeactivated();
    const ImVec2 dateRectMin = ImGui::GetItemRectMin();
    const ImVec2 dateRectMax = ImGui::GetItemRectMax();
    // A caret put back after an Enter that did not read is the same session: it keeps the typed text and its error.
    const bool cameBackAfterEnter = g_typedDate.Id == dateId && g_typedDate.RefocusFrames > 0;
    if (ImGui::IsItemActivated() && !dateEdited && !cameBackAfterEnter) {
        g_typedDate = TypedDateState();
        g_typedDate.Id = dateId;
        g_typedDate.Text = dateBuf;
    }
    if (dateEdited) {
        if (g_typedDate.Id != dateId) {
            g_typedDate = TypedDateState();
            g_typedDate.Id = dateId;
        }
        g_typedDate.Text = dateBuf;
        g_typedDate.Edited = true;
    }
    TrackerParseExplain::CivilDate typedDate;
    if (ResolveTypedDate(dateId, enterPressed, dateActive, dateDeactivated, dateRectMin, dateRectMax, typedDate)) {
        parsed.Year = typedDate.Year;
        parsed.Month = typedDate.Month;
        parsed.Day = typedDate.Day;
        ioValue = FormatJiraDateOrDateTimeForApi(!isDateTime, parsed);
        valueChanged = true;
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
