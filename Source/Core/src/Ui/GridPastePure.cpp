#include "Ui/GridPastePure.h"

#include "CompactDateFormat.h" // TryParseJiraDateTime, FormatJiraDateOrDateTimeForApi
#include "StringUtil.h"
#include "Tracker/TrackerFieldPayloadPure.h"
#include "Tracker/TrackerFieldValueUtils.h"
#include "Ui/BulkFieldEditPure.h"

#include <algorithm>
#include <utility>

namespace smatchet {
namespace grid {
namespace paste {

namespace {

using TrackerParseExplain::CivilDate;
using TrackerParseExplain::DateOrder;

std::string WithoutCarriageReturns(const std::string& text) {
    std::string out = text;
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    return out;
}

// A date or date-time in the form trackers store it, naming a day that exists.
bool ReadStoredDate(const std::string& text, const CivilDate& today, ParsedJiraDateTime& out) {
    ParsedJiraDateTime parsed;
    if (!TryParseJiraDateTime(text, parsed)) {
        return false;
    }
    const std::string day = TrimCopy(text).substr(0, 10);
    if (!TrackerParseExplain::ParseDateInput(day, today, DateOrder::MonthFirst).Ok) {
        return false;
    }
    out = parsed;
    return true;
}

bool ReadPastedDate(const std::string& text, const std::string& exact, const CivilDate& today,
                    ParsedJiraDateTime& out) {
    if (ReadStoredDate(exact, today, out) || ReadStoredDate(text, today, out)) {
        return true;
    }
    // A signed offset is how the grid shows an old or far date ("-2w"); it no longer says which day.
    if (text[0] == '+' || text[0] == '-') {
        return false;
    }
    const TrackerParseExplain::DateParse typed =
        TrackerParseExplain::ParseDateInput(text, today, DateOrder::MonthFirst);
    if (!typed.Ok) {
        return false;
    }
    out = ParsedJiraDateTime();
    out.Year = typed.Date.Year;
    out.Month = typed.Date.Month;
    out.Day = typed.Date.Day;
    return true;
}

// A day pasted onto a date-time field keeps the cell's time of day and zone; a cell with none gets noon UTC, which is
// the same calendar day almost everywhere.
void KeepTimeOfDay(const std::string& current, ParsedJiraDateTime& value) {
    if (value.HasWallTime) {
        return;
    }
    ParsedJiraDateTime held;
    if (TryParseJiraDateTime(current, held) && held.HasWallTime) {
        held.Year = value.Year;
        held.Month = value.Month;
        held.Day = value.Day;
        value = held;
        return;
    }
    value.Hour = 12;
    value.HasWallTime = true;
}

bool ResolveDate(bool dateOnly, const PasteValueInput& in, const CivilDate& today, std::vector<std::string>& out) {
    const std::string text = TrimCopy(in.Text);
    if (text.empty()) {
        return true; // no value clears the date, as the picker's Clear does
    }
    ParsedJiraDateTime value;
    if (!ReadPastedDate(text, in.Exact, today, value)) {
        return false;
    }
    if (!dateOnly) {
        KeepTimeOfDay(in.Current, value);
    }
    out.push_back(FormatJiraDateOrDateTimeForApi(dateOnly, value));
    return true;
}

bool ResolveDuration(const std::string& raw, std::vector<std::string>& out) {
    const std::string text = TrimCopy(raw);
    if (text.empty()) {
        out.push_back(std::string());
        return true;
    }
    const TrackerParseExplain::DurationParse parsed =
        TrackerParseExplain::ParseDurationInput(text, TrackerParseExplain::BareNumberUnit::Minutes);
    if (!parsed.Ok) {
        return false;
    }
    out.push_back(parsed.Canonical);
    return true;
}

bool ResolveChoice(const TrackerField& field, bulk::BulkFieldKind kind, const std::string& raw,
                   std::vector<std::string>& out) {
    const std::string text = TrimCopy(raw);
    if (text.empty()) {
        // A ticket always has a status and a type.
        return field.Family != TrackerFieldFamily::Status && field.Family != TrackerFieldFamily::IssueType;
    }
    std::vector<std::string> values = bulk::BeforeValuesFor(field, kind, text);
    if (values.empty() || (kind != bulk::BulkFieldKind::Labels && !bulk::AllEntriesResolve(field, values))) {
        return false;
    }
    out = std::move(values);
    return true;
}

// A plain decimal number: an optional sign, digits, at most one point, and at least one digit.
bool ReadsAsNumber(const std::string& text) {
    std::size_t at = (!text.empty() && (text[0] == '+' || text[0] == '-')) ? 1 : 0;
    bool digit = false;
    bool point = false;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c == '.' && !point) {
            point = true;
        } else {
            return false;
        }
    }
    return digit;
}

// A number field: the trimmed number, or an empty text that clears it. Other text is a column header or a note.
bool ResolveNumber(const std::string& raw, std::vector<std::string>& out) {
    const std::string text = TrimCopy(raw);
    if (!text.empty() && !ReadsAsNumber(text)) {
        return false;
    }
    out.push_back(text);
    return true;
}

} // namespace

bool IsOwnCopy(const CopyRecord& copy, const std::string& clipboard) {
    return !copy.Tsv.empty() && WithoutCarriageReturns(copy.Tsv) == WithoutCarriageReturns(clipboard);
}

std::string ExactAt(const CopyRecord& copy, int row, int col) {
    if (row < 0 || col < 0 || static_cast<std::size_t>(row) >= copy.Exact.size()) {
        return std::string();
    }
    const std::vector<std::string>& cells = copy.Exact[static_cast<std::size_t>(row)];
    return static_cast<std::size_t>(col) < cells.size() ? cells[static_cast<std::size_t>(col)] : std::string();
}

bool IsFreeValueField(const TrackerField& field) {
    if (!field.AllowedValueOptions.empty()) {
        return false;
    }
    return field.Family == TrackerFieldFamily::Text || field.Family == TrackerFieldFamily::Number ||
           TrackerFieldValueUtils::IsTimeDurationField(field.Id);
}

bool ResolveCellText(const TrackerField& field, const PasteValueInput& in, const TrackerParseExplain::CivilDate& today,
                     std::vector<std::string>& out) {
    out.clear();
    if (field.ReadOnly || TrackerFieldPayloadPure::FieldUsesAdfDocument(field)) {
        return false;
    }
    if (field.Type == "date" || field.Type == "datetime") {
        return ResolveDate(field.Type == "date", in, today, out);
    }
    if (TrackerFieldValueUtils::IsTimeDurationField(field.Id)) {
        return ResolveDuration(in.Text, out);
    }
    const bulk::BulkFieldKind kind = bulk::ClassifyBulkField(field);
    if (kind != bulk::BulkFieldKind::Unsupported) {
        return ResolveChoice(field, kind, in.Text, out);
    }
    if (!IsFreeValueField(field)) {
        return false;
    }
    if (field.Family == TrackerFieldFamily::Number) {
        return ResolveNumber(in.Text, out);
    }
    out.push_back(in.Text);
    return true;
}

} // namespace paste
} // namespace grid
} // namespace smatchet
