#include "TrackerDateTimeFieldEditor.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace {

static int InputTextCallback_ClearSelectOnEditOpen(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways || !data->UserData) {
        return 0;
    }
    auto* state = static_cast<SpreadsheetState*>(data->UserData);
    if (!state->PendingGridInputTextDeselect) {
        return 0;
    }
    const bool fullRange =
        data->BufTextLen > 0 && data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen;
    if (!data->EventActivated && !fullRange) {
        return 0;
    }
    const int end = data->BufTextLen;
    data->SetSelection(end, end);
    state->PendingGridInputTextDeselect = false;
    return 0;
}

#if defined(_WIN32)
std::time_t TimeGmPortable(std::tm* tmUtc) { return _mkgmtime(tmUtc); }
#else
std::time_t TimeGmPortable(std::tm* tmUtc) { return timegm(tmUtc); }
#endif

bool IsLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

int DaysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 31;
    }
    int d = kDays[month - 1];
    if (month == 2 && IsLeapYear(year)) {
        d = 29;
    }
    return d;
}

void DecMonth(int& year, int& month) {
    if (--month < 1) {
        month = 12;
        --year;
    }
}

void IncMonth(int& year, int& month) {
    if (++month > 12) {
        month = 1;
        ++year;
    }
}

/** Weekday of first of month: 0 = Sunday .. 6 = Saturday (UTC). */
int FirstOfMonthWeekday0Sun(int year, int month) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = 1;
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = 0;
    const std::time_t tt = TimeGmPortable(&t);
    if (tt == static_cast<std::time_t>(-1)) {
        return 0;
    }
    std::tm r{};
#if defined(_WIN32)
    if (gmtime_s(&r, &tt) != 0) {
        return 0;
    }
#else
    if (gmtime_r(&tt, &r) == nullptr) {
        return 0;
    }
#endif
    return r.tm_wday;
}

ParsedJiraDateTime TodayUtcParsed(bool includeTime) {
    ParsedJiraDateTime p{};
    const std::time_t now = std::time(nullptr);
    std::tm r{};
#if defined(_WIN32)
    if (gmtime_s(&r, &now) != 0) {
        return p;
    }
#else
    if (gmtime_r(&now, &r) == nullptr) {
        return p;
    }
#endif
    p.Year = r.tm_year + 1900;
    p.Month = r.tm_mon + 1;
    p.Day = r.tm_mday;
    p.HasWallTime = includeTime;
    p.Hour = includeTime ? r.tm_hour : 0;
    p.Minute = includeTime ? r.tm_min : 0;
    p.Second = includeTime ? r.tm_sec : 0;
    p.HasTimeZoneSuffix = false;
    p.TimeZoneWasZ = false;
    p.OffsetSec = 0;
    return p;
}

void ClampDayToMonth(ParsedJiraDateTime& w, int year, int month) {
    const int dim = DaysInMonth(year, month);
    if (w.Day > dim) {
        w.Day = dim;
    }
    if (w.Day < 1) {
        w.Day = 1;
    }
}

std::string FormatFriendlyDate(const ParsedJiraDateTime& p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d/%d/%04d", p.Month, p.Day, p.Year);
    return std::string(buf);
}

bool ParseFriendlyDate(const std::string& s, int& outY, int& outM, int& outD) {
    int m = 0, d = 0, y = 0;
    if (std::sscanf(s.c_str(), "%d/%d/%d", &m, &d, &y) == 3) {
        if (m >= 1 && m <= 12 && d >= 1 && d <= 31 && y >= 1900 && y <= 3000) {
            outY = y;
            outM = m;
            outD = d;
            return true;
        }
    }
    return false;
}

std::string FormatFriendlyTime(const ParsedJiraDateTime& p) {
    int h = p.Hour;
    const char* ampm = "AM";
    if (h >= 12) {
        ampm = "PM";
        if (h > 12) h -= 12;
    }
    if (h == 0) h = 12;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d %s", h, p.Minute, ampm);
    return std::string(buf);
}

bool ParseFriendlyTime(const std::string& s, int& outH, int& outM) {
    int h = 0, m = 0;
    char ampm[8] = "";
    if (std::sscanf(s.c_str(), "%d:%d %7s", &h, &m, ampm) == 3) {
        std::string ampmStr = ampm;
        std::transform(ampmStr.begin(), ampmStr.end(), ampmStr.begin(), [](unsigned char c) {
            return std::toupper(c);
        });
        if (ampmStr == "PM" && h < 12) h += 12;
        if (ampmStr == "AM" && h == 12) h = 0;
        if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            outH = h;
            outM = m;
            return true;
        }
    } else if (std::sscanf(s.c_str(), "%d:%d", &h, &m) == 2) {
        if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            outH = h;
            outM = m;
            return true;
        }
    }
    return false;
}

const char* const kFullMonthNames[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

enum class PickerAction {
    None,
    Apply,
    Clear,
    Cancel
};

PickerAction DrawCalendarPicker(
    ParsedJiraDateTime& working,
    int& viewYear,
    int& viewMonth,
    bool& forceTextMode,
    bool isDateOnly,
    const std::string& originalValue,
    char* textModeBuffer,
    size_t textModeBufferSize,
    ImGuiInputTextCallback callback = nullptr,
    void* callbackUserData = nullptr,
    bool isDropdown = false
) {
    if (forceTextMode) {
        ImGui::TextUnformatted("Edit raw ISO string:");
        ImGui::SetNextItemWidth(420.0f);
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
        if (callback) {
            flags |= ImGuiInputTextFlags_CallbackAlways;
        }
        const bool submitted = ImGui::InputText("##rawiso", textModeBuffer, textModeBufferSize, flags, callback, callbackUserData);
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
    } else {
        if (!isDropdown) {
            std::string initialText = originalValue.empty() ? "" : FormatCompactJiraDateForDisplay(originalValue, "absolute_friendly");
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
                    std::snprintf(temp, sizeof(temp), "%04d-%02d-%02d, %02d:%02d:%02d UTC",
                                  working.Year, working.Month, working.Day,
                                  working.Hour, working.Minute, working.Second);
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

        // Header chevrons (<<, <, Title, >, >>)
        char title[64];
        if (viewMonth >= 1 && viewMonth <= 12) {
            std::snprintf(title, sizeof(title), "%s %d", kFullMonthNames[viewMonth - 1], viewYear);
        } else {
            std::snprintf(title, sizeof(title), "%d-%02d", viewYear, viewMonth);
        }

        const float calendarWidth = 220.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
        
        // Left chevrons
        if (ImGui::Button("<<")) {
            viewYear--;
        }
        ImGui::SameLine();
        if (ImGui::Button("<")) {
            DecMonth(viewYear, viewMonth);
        }
        
        // Centered Title
        float titleWidth = ImGui::CalcTextSize(title).x;
        float titlePos = (calendarWidth - titleWidth) * 0.5f;
        ImGui::SameLine(titlePos);
        ImGui::TextUnformatted(title);
        
        // Right chevrons
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

        // Sunday-first columns
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
                ImGui::PushID(row * 10 + col);
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
                    const bool selected =
                        (working.Year == viewYear && working.Month == viewMonth && working.Day == d);
                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(31.0f/255.0f, 116.0f/255.0f, 236.0f/255.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(51.0f/255.0f, 136.0f/255.0f, 1.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(21.0f/255.0f, 96.0f/255.0f, 216.0f/255.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    }
                    if (ImGui::SmallButton(std::to_string(d).c_str())) {
                        working.Year = viewYear;
                        working.Month = viewMonth;
                        working.Day = d;
                        if (isDropdown) {
                            if (selected) ImGui::PopStyleColor(4);
                            ImGui::PopID();
                            return PickerAction::Apply;
                        }
                    }
                    if (selected) {
                        ImGui::PopStyleColor(4);
                    }
                }
                ImGui::PopID();
                ++dayCounter;
            }
        }

        if (!isDropdown) {
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
        }
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
                               const std::string& dateFormatOption, int thresholdDays) {
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
        std::string fmt = dateFormatOption;
        int thresh = thresholdDays;
        if (fmt.empty() || thresh <= 0) {
            const auto cfg = ConfigManager::Load();
            if (fmt.empty()) fmt = cfg.DateFormatOption;
            if (thresh <= 0) thresh = cfg.DateCompactRelativeThresholdDays;
        }
        std::string display = FormatCompactJiraDateForDisplay(currentValue, fmt, thresh);
        if (display.empty()) {
            display = currentValue;
        }
        auto it = std::find_if(display.begin(), display.end(), [](char c) {
            return c == '\n' || c == '\r';
        });
        if (it != display.end()) {
            display.erase(it, display.end());
        }
        if (display.empty()) {
            display = "";
        }
        const bool blankValue = std::all_of(currentValue.begin(), currentValue.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
        if (ImGui::Selectable((display + itemId).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            // Empty due date (and similar) would otherwise require double-click like populated cells.
            if (blankValue || ImGui::IsMouseDoubleClicked(0)) {
                state.StartEditingField(ticket.id, field.Id, currentValue);
            }
        }
        ImGui::PopID();
        return;
    }

    const bool editJustStarted = state.EditJustStarted;
    if (editJustStarted) {
        ParsedJiraDateTime parsed;
        if (TryParseJiraDateTime(currentValue, parsed)) {
            s_working = parsed;
            s_forceTextMode = false;
            s_viewYear = parsed.Year;
            s_viewMonth = parsed.Month;
            ClampDayToMonth(s_working, s_viewYear, s_viewMonth);
        } else if (currentValue.empty()) {
            s_working = TodayUtcParsed(!isDateOnly);
            s_forceTextMode = false;
            s_viewYear = s_working.Year;
            s_viewMonth = s_working.Month;
        } else {
            s_forceTextMode = true;
        }
        ImGui::SetNextWindowPos(ImGui::GetIO().MousePos, ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
    }
    if (!ImGui::IsPopupOpen("picker")) {
        ImGui::OpenPopup("picker");
    }

    const bool modalOpen = ImGui::BeginPopupModal("picker", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (modalOpen) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            state.ClearEditing();
            ImGui::CloseCurrentPopup();
        }
        
        PickerAction action = DrawCalendarPicker(
            s_working, s_viewYear, s_viewMonth, s_forceTextMode, isDateOnly, currentValue,
            state.EditBuffer, sizeof(state.EditBuffer),
            InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&state)
        );
        
        if (action == PickerAction::Apply) {
            ClampDayToMonth(s_working, s_working.Year, s_working.Month);
            const std::string canon = FormatJiraDateOrDateTimeForApi(isDateOnly, s_working);
            queue({canon});
            state.ClearEditing();
            ImGui::CloseCurrentPopup();
        } else if (action == PickerAction::Clear) {
            queue({});
            state.ClearEditing();
            ImGui::CloseCurrentPopup();
        } else if (action == PickerAction::Cancel) {
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

bool RenderGenericDatePicker(const char* label, std::string& ioValue, bool isDateTime) {
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
    
    const float totalWidth = 340.0f;
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
        
        PickerAction action = DrawCalendarPicker(
            s_genWorking, s_genViewYear, s_genViewMonth, s_genForceTextMode, !isDateTime, ioValue,
            genRawBuf, sizeof(genRawBuf), nullptr, nullptr, true
        );
        
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
    
    // Column 2 (Time Field): Side-by-side with close button, only if isDateTime is true
    if (isDateTime) {
        ImGui::SameLine(0.0f, 6.0f);
        
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
        ImGui::SetNextItemWidth(colWidth - 26.0f);
        if (ImGui::InputText("##FriendlyTime", timeBuf, sizeof(timeBuf))) {
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
                        if (displayH > 12) displayH -= 12;
                    }
                    if (displayH == 0) displayH = 12;
                    
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
    
    ImGui::PopID();
    return valueChanged;
}

} // namespace TrackerDateTimeFieldEditor







