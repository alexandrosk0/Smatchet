#include "TrackerDateTimeFieldEditor.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "imgui.h"

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

const char* const kMonthAbbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

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
        for (size_t i = 0; i < display.size(); ++i) {
            if (display[i] == '\n' || display[i] == '\r') {
                display.erase(i);
                break;
            }
        }
        if (display.empty()) {
            display = "(empty)";
        }
        const bool blankValue = [&] {
            for (unsigned char ch : currentValue) {
                if (std::isspace(ch) == 0) {
                    return false;
                }
            }
            return true;
        }();
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
        if (s_forceTextMode) {
            ImGui::TextUnformatted("Unparseable value; edit raw ISO string:");
            ImGui::SetNextItemWidth(420.0f);
            const bool submitted =
                ImGui::InputText("##rawiso", state.EditBuffer, sizeof(state.EditBuffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                 InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&state));
            ParsedJiraDateTime tmp;
            const bool canParse = TryParseJiraDateTime(state.EditBuffer, tmp);
            if (!canParse) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Apply parsed value") || (submitted && canParse)) {
                const std::string canon = FormatJiraDateOrDateTimeForApi(isDateOnly, tmp);
                queue({canon});
                state.ClearEditing();
                ImGui::CloseCurrentPopup();
            }
            if (!canParse) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                state.ClearEditing();
                ImGui::CloseCurrentPopup();
            }
        } else {
            // Show original and real-time selected values at the top of the picker
            std::string initialText = currentValue.empty() ? "(empty)" : FormatCompactJiraDateForDisplay(currentValue, "absolute_friendly");
            if (initialText.empty() && !currentValue.empty()) {
                initialText = currentValue;
            }

            std::string workingText;
            if (s_working.Year > 0) {
                if (isDateOnly) {
                    char temp[64];
                    std::snprintf(temp, sizeof(temp), "%04d-%02d-%02d", s_working.Year, s_working.Month, s_working.Day);
                    workingText = temp;
                } else {
                    char temp[128];
                    std::snprintf(temp, sizeof(temp), "%04d-%02d-%02d, %02d:%02d:%02d UTC",
                                  s_working.Year, s_working.Month, s_working.Day,
                                  s_working.Hour, s_working.Minute, s_working.Second);
                    workingText = temp;
                }
            } else {
                workingText = "(empty)";
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

            if (ImGui::Button("<")) {
                DecMonth(s_viewYear, s_viewMonth);
            }
            ImGui::SameLine();
            char title[64];
            if (s_viewMonth >= 1 && s_viewMonth <= 12) {
                std::snprintf(title, sizeof(title), "%s %d", kMonthAbbr[s_viewMonth - 1], s_viewYear);
            } else {
                std::snprintf(title, sizeof(title), "%d-%02d", s_viewYear, s_viewMonth);
            }
            ImGui::TextUnformatted(title);
            ImGui::SameLine();
            if (ImGui::Button(">")) {
                IncMonth(s_viewYear, s_viewMonth);
            }

            const int wday0Sun = FirstOfMonthWeekday0Sun(s_viewYear, s_viewMonth);
            const int startOffset = (wday0Sun + 6) % 7; // Monday-first column index for day 1
            const int dim = DaysInMonth(s_viewYear, s_viewMonth);

            const char* dow[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
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
                    if (dayCounter < 1 || dayCounter > dim) {
                        ImGui::Dummy(ImVec2(28.0f, 22.0f));
                    } else {
                        const int d = dayCounter;
                        const bool selected =
                            (s_working.Year == s_viewYear && s_working.Month == s_viewMonth && s_working.Day == d);
                        if (selected) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                        }
                        if (ImGui::SmallButton(std::to_string(d).c_str())) {
                            s_working.Year = s_viewYear;
                            s_working.Month = s_viewMonth;
                            s_working.Day = d;
                        }
                        if (selected) {
                            ImGui::PopStyleColor();
                        }
                    }
                    ImGui::PopID();
                    ++dayCounter;
                }
            }

            if (!isDateOnly) {
                ImGui::Separator();
                int h = s_working.Hour;
                int m = s_working.Minute;
                int sec = s_working.Second;
                ImGui::TextUnformatted("Time (UTC wall)");
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragInt("##h", &h, 0.25f, 0, 23, "%02d h");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragInt("##m", &m, 0.25f, 0, 59, "%02d m");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragInt("##s", &sec, 0.25f, 0, 59, "%02d s");
                s_working.HasWallTime = true;
                s_working.Hour = (std::max)(0, (std::min)(23, h));
                s_working.Minute = (std::max)(0, (std::min)(59, m));
                s_working.Second = (std::max)(0, (std::min)(59, sec));
            } else {
                s_working.HasWallTime = false;
                s_working.Hour = 0;
                s_working.Minute = 0;
                s_working.Second = 0;
            }

            ImGui::Separator();
            if (ImGui::Button("Apply")) {
                ClampDayToMonth(s_working, s_working.Year, s_working.Month);
                const std::string canon = FormatJiraDateOrDateTimeForApi(isDateOnly, s_working);
                queue({canon});
                state.ClearEditing();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                queue({});
                state.ClearEditing();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                state.ClearEditing();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Raw ISO…")) {
                const std::string preview = FormatJiraDateOrDateTimeForApi(isDateOnly, s_working);
                std::snprintf(state.EditBuffer, sizeof(state.EditBuffer), "%s", preview.c_str());
                state.EditBuffer[sizeof(state.EditBuffer) - 1] = '\0';
                s_forceTextMode = true;
            }
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

} // namespace TrackerDateTimeFieldEditor







