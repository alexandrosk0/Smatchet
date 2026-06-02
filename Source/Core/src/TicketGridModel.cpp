#include "TicketGridModel.h"

#include "CompactDateFormat.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerLabelsEditor.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iterator>
#include <unordered_set>

namespace {

long long ParseDurationToSecondsForSort(const std::string& input) {
    constexpr long long kSecondsPerHour = 3600;
    constexpr long long kSecondsPerDay = 8 * kSecondsPerHour;
    constexpr long long kSecondsPerWeek = 5 * kSecondsPerDay;
    auto trim = [](const std::string& s) {
        size_t a = 0;
        size_t b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) {
            ++a;
        }
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
            --b;
        }
        return s.substr(a, b - a);
    };
    std::string s = trim(input);
    if (s.empty()) {
        return 0;
    }
    size_t pos = 0;
    try {
        long long v = std::stoll(s, &pos, 10);
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
            ++pos;
        }
        if (pos >= s.size()) {
            return v;
        }
    } catch (...) {
    }
    pos = 0;
    long long total = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
            ++pos;
        }
        if (pos >= s.size()) {
            break;
        }
        long long num = 1;
        if (std::isdigit(static_cast<unsigned char>(s[pos]))) {
            size_t next = 0;
            try {
                num = std::stoll(s.substr(pos), &next, 10);
                pos += next;
            } catch (...) {
                break;
            }
        }
        if (pos >= s.size()) {
            total += num;
            break;
        }
        const char u = s[pos];
        if (u == 'w' || u == 'W') {
            total += num * kSecondsPerWeek;
            ++pos;
        } else if (u == 'd' || u == 'D') {
            total += num * kSecondsPerDay;
            ++pos;
        } else if (u == 'h' || u == 'H') {
            total += num * kSecondsPerHour;
            ++pos;
        } else if (u == 'm' || u == 'M') {
            total += num * 60LL;
            ++pos;
        } else {
            total += num;
        }
    }
    return total;
}

const std::unordered_set<std::string> kTimeTrackingFieldIds = {
    "timeoriginalestimate",          "timeestimate",          "timespent",
    "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent"};

const std::unordered_set<std::string> kDateFieldIds = {"created", "updated", "duedate"};

std::string TrimFieldId(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\n' || value[start] == '\r')) {
        ++start;
    }
    while (end > start &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\n' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(start, end - start);
}

TicketGridColumn::RenderPlan ResolveRenderPlan(const std::string& fieldId, const TrackerField* field) {
    if (fieldId == "timespent" || (field && field->Id == "timespent")) {
        return TicketGridColumn::RenderPlan::SpecialTimeSpent;
    }
    if (field == nullptr) {
        if (IsAttachmentFieldId(fieldId)) {
            return TicketGridColumn::RenderPlan::SpecialAttachment;
        }
        if (TrackerGridFieldDisplay::IsWatchersColumnId(fieldId)) {
            return TicketGridColumn::RenderPlan::SpecialWatchers;
        }
        if (TrackerGridFieldDisplay::IsVotesColumnId(fieldId)) {
            return TicketGridColumn::RenderPlan::SpecialVotes;
        }
        if (TrackerGridFieldDisplay::IsWorklogColumnId(fieldId)) {
            return TicketGridColumn::RenderPlan::SpecialWorklog;
        }
        if (TrackerGridFieldDisplay::IsProgressStyleColumnId(fieldId)) {
            return TicketGridColumn::RenderPlan::SpecialProgress;
        }
        if (TrackerGridFieldDisplay::IsIssueRestrictionColumnId(fieldId)) {
            return TicketGridColumn::RenderPlan::SpecialIssueRestriction;
        }
        return TicketGridColumn::RenderPlan::PlainText;
    }

    if (IsAttachmentFieldId(field->Id)) {
        return TicketGridColumn::RenderPlan::SpecialAttachment;
    }
    if (TrackerGridFieldDisplay::IsWatchersColumnId(field->Id)) {
        return TicketGridColumn::RenderPlan::SpecialWatchers;
    }
    if (TrackerGridFieldDisplay::IsVotesColumnId(field->Id)) {
        return TicketGridColumn::RenderPlan::SpecialVotes;
    }
    if (TrackerGridFieldDisplay::IsWorklogColumnId(field->Id)) {
        return TicketGridColumn::RenderPlan::SpecialWorklog;
    }
    if (TrackerGridFieldDisplay::IsProgressDisplayField(field)) {
        return TicketGridColumn::RenderPlan::SpecialProgress;
    }
    if (TrackerGridFieldDisplay::IsIssueRestrictionField(field)) {
        return TicketGridColumn::RenderPlan::SpecialIssueRestriction;
    }
    if (field->ReadOnly) {
        return TicketGridColumn::RenderPlan::PlainText;
    }
    if (TrackerLabelsEditor::IsLabelsField(field->Id)) {
        return TicketGridColumn::RenderPlan::Labels;
    }
    if (field->Family == TrackerFieldFamily::CascadingSelect) {
        return TicketGridColumn::RenderPlan::Cascading;
    }
    if ((field->Family == TrackerFieldFamily::SelectMulti || field->Family == TrackerFieldFamily::StructuredMulti ||
         field->Family == TrackerFieldFamily::UserMulti) &&
        !field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::MultiSelect;
    }
    if ((field->Family == TrackerFieldFamily::SelectSingle || field->Family == TrackerFieldFamily::StructuredSingle ||
         field->Family == TrackerFieldFamily::UserSingle || field->Family == TrackerFieldFamily::Status ||
         field->Family == TrackerFieldFamily::IssueType) &&
        !field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::SingleSelect;
    }
    if (field->IsArray && !field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::MultiSelect;
    }
    // Fallback B: components on an unresolvable-JQL view (filter-id / cross-project / non-`project=`)
    // can't be enriched, so AllowedValueOptions stays empty. Still render an (empty) MultiSelect
    // dropdown rather than a text editor — it lazily populates once a project-scoped catalog lands.
    // The common `project = X` case populates real options via the scoped catalog save/load path
    // and matches the non-empty MultiSelect branch above; this is the degraded-only path.
    if (field->IsArray &&
        (field->Family == TrackerFieldFamily::SelectMulti || ToLowerAsciiCopy(field->ItemsType) == "component")) {
        return TicketGridColumn::RenderPlan::MultiSelect;
    }
    if (!field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::SingleSelect;
    }
    if (TrackerDateTimeFieldEditor::IsTrackerDateTimePickerField(*field)) {
        return TicketGridColumn::RenderPlan::DateTimeEditor;
    }
    return TicketGridColumn::RenderPlan::TextEditor;
}

bool RequiresAllowEditsCheck(TicketGridColumn::RenderPlan plan) {
    return plan == TicketGridColumn::RenderPlan::Labels || plan == TicketGridColumn::RenderPlan::Cascading ||
           plan == TicketGridColumn::RenderPlan::MultiSelect || plan == TicketGridColumn::RenderPlan::SingleSelect ||
           plan == TicketGridColumn::RenderPlan::DateTimeEditor || plan == TicketGridColumn::RenderPlan::TextEditor;
}

} // namespace

TicketGridColumn::RenderPlan ResolveTicketGridRenderPlan(const std::string& fieldId, const TrackerField* field) {
    return ResolveRenderPlan(fieldId, field);
}

/** Whole-string numeric parse; never throws (stable_sort comparator must not throw). */
static bool ParseWholeInt64Dec(const std::string& s, long long& out) {
    errno = 0;
    char* end = nullptr;
    const char* const c = s.c_str();
    out = std::strtoll(c, &end, 10);
    if (end != c + s.size()) {
        return false;
    }
    return errno != ERANGE;
}

static bool ParseWholeDouble(const std::string& s, double& out) {
    errno = 0;
    char* end = nullptr;
    const char* const c = s.c_str();
    out = std::strtod(c, &end);
    if (end != c + s.size()) {
        return false;
    }
    return errno != ERANGE;
}

int CompareFieldValuesForSort(const std::string& fieldId, const TrackerField* fieldMeta, const std::string& aVal,
                              const std::string& bVal, int sortDirection) {
    const bool aEmpty = aVal.empty();
    const bool bEmpty = bVal.empty();
    if (aEmpty && bEmpty) {
        return 0;
    }
    if (aEmpty) {
        return (sortDirection == 1) ? 1 : -1;
    }
    if (bEmpty) {
        return (sortDirection == 1) ? -1 : 1;
    }

    if (fieldId == "key" || fieldId == "issuekey") {
        if (CompareIssueKeyNatural(aVal, bVal))
            return -1;
        if (CompareIssueKeyNatural(bVal, aVal))
            return 1;
        return 0;
    }

    if (kTimeTrackingFieldIds.count(fieldId)) {
        const long long sa = ParseDurationToSecondsForSort(aVal);
        const long long sb = ParseDurationToSecondsForSort(bVal);
        if (sa != sb) {
            return (sa < sb) ? -1 : 1;
        }
        return 0;
    }
    if (kDateFieldIds.count(fieldId) || (fieldMeta && (fieldMeta->Type == "date" || fieldMeta->Type == "datetime"))) {
        const int cmp = aVal.compare(bVal);
        if (cmp != 0) {
            return (cmp < 0) ? -1 : 1;
        }
        return 0;
    }
    if (fieldMeta && fieldMeta->Type == "number") {
        double da = 0;
        double db = 0;
        const bool aNum = ParseWholeDouble(aVal, da);
        const bool bNum = ParseWholeDouble(bVal, db);
        if (aNum && bNum) {
            if (da != db) {
                return (da < db) ? -1 : 1;
            }
            return 0;
        }
    } else {
        long long na = 0;
        long long nb = 0;
        const bool aInt = ParseWholeInt64Dec(aVal, na);
        const bool bInt = ParseWholeInt64Dec(bVal, nb);
        if (aInt && bInt) {
            if (na != nb) {
                return (na < nb) ? -1 : 1;
            }
            return 0;
        }
    }
    auto ciCompare = [](const std::string& x, const std::string& y) {
        const size_t n = (std::min)(x.size(), y.size());
        for (size_t i = 0; i < n; ++i) {
            const int cxa = std::tolower(static_cast<unsigned char>(x[i]));
            const int cxb = std::tolower(static_cast<unsigned char>(y[i]));
            if (cxa != cxb) {
                return (cxa < cxb) ? -1 : 1;
            }
        }
        if (x.size() != y.size()) {
            return (x.size() < y.size()) ? -1 : 1;
        }
        return 0;
    };
    return ciCompare(aVal, bVal);
}

bool IsTrackerDateOrDateTimeField(const std::string& fieldId, const TrackerField* field) {
    if (kDateFieldIds.count(fieldId)) {
        return true;
    }
    if (field && (field->Type == "date" || field->Type == "datetime" || field->Family == TrackerFieldFamily::Date ||
                  field->Family == TrackerFieldFamily::DateTime)) {
        return true;
    }
    // Case-insensitive heuristics for auto-detection of date-like fields
    const std::string idLower = ToLowerAsciiCopy(fieldId);
    if (idLower.find("date") != std::string::npos || idLower.find("time") != std::string::npos ||
        idLower.find("created") != std::string::npos || idLower.find("updated") != std::string::npos ||
        idLower.find("modified") != std::string::npos || idLower.find("viewed") != std::string::npos ||
        idLower.find("changed") != std::string::npos || idLower.find("resolved") != std::string::npos ||
        idLower.find("duedate") != std::string::npos) {
        return true;
    }
    if (field) {
        const std::string nameLower = ToLowerAsciiCopy(field->Name);
        if (nameLower.find("date") != std::string::npos || nameLower.find("time") != std::string::npos ||
            nameLower.find("created") != std::string::npos || nameLower.find("updated") != std::string::npos ||
            nameLower.find("modified") != std::string::npos || nameLower.find("viewed") != std::string::npos ||
            nameLower.find("changed") != std::string::npos || nameLower.find("resolved") != std::string::npos ||
            nameLower.find("due date") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool IsAttachmentFieldId(const std::string& fieldId) {
    const std::string lower = ToLowerAsciiCopy(fieldId);
    return lower == "attachment" || lower == "attachments";
}

std::string DisplayValueForTrackerDateField(const std::string& fieldId, const TrackerField* field,
                                            const std::string& currentValue, const std::string& dateFormatOption,
                                            int thresholdDays) {
    bool isDate = IsTrackerDateOrDateTimeField(fieldId, field);
    if (!isDate) {
        ParsedJiraDateTime dummy;
        if (TryParseJiraDateTime(currentValue, dummy)) {
            isDate = true;
        }
    }
    if (!isDate) {
        return currentValue;
    }
    // Use caller-supplied format params if provided; only hit disk when called from non-hot paths.
    std::string fmt = dateFormatOption;
    int thresh = thresholdDays;
    if (fmt.empty() || thresh <= 0) {
        const auto cfg = ConfigManager::Load();
        if (fmt.empty())
            fmt = cfg.DateFormatOption;
        if (thresh <= 0)
            thresh = cfg.DateCompactRelativeThresholdDays;
    }
    const std::string compact = FormatCompactJiraDateForDisplay(currentValue, fmt, thresh);
    return compact.empty() ? currentValue : compact;
}

TrackerFieldCatalogIndex::TrackerFieldCatalogIndex(const std::vector<TrackerField>& fields) {
    for (const auto& field : fields) {
        FieldById[field.Id] = &field;
    }
}

const TrackerField* TrackerFieldCatalogIndex::Find(const std::string& fieldId) const {
    const auto it = FieldById.find(fieldId);
    return it == FieldById.end() ? nullptr : it->second;
}

std::string TrackerFieldCatalogIndex::DisplayName(const std::string& fieldId) const {
    if (fieldId == "history") {
        return "History";
    }
    const TrackerField* field = Find(fieldId);
    return field ? field->Name : fieldId;
}

std::vector<TicketGridColumn> TicketGridColumnsBuilder::Build(const ViewDefinition& view,
                                                              const TrackerFieldCatalogIndex& catalog) {
    std::vector<TicketGridColumn> columns;
    std::vector<TicketGridColumn> allColumns;
    allColumns.push_back({TicketGridColumn::Kind::Id, "id", "ID", std::string()});

    std::unordered_set<std::string> seenFieldIds;
    for (const auto& rawFieldId : view.Fields) {
        const std::string fieldId = TrimFieldId(rawFieldId);
        if (fieldId.empty() || !seenFieldIds.insert(fieldId).second) {
            continue;
        }

        TicketGridColumn column;
        column.ColumnKind = TicketGridColumn::Kind::FieldValue;
        column.Key = "field:" + fieldId;
        column.FieldId = fieldId;
        column.Label = catalog.DisplayName(fieldId);
        const TrackerField* field = catalog.Find(fieldId);
        column.Plan = ResolveRenderPlan(fieldId, field);
        column.IsDateLike = IsTrackerDateOrDateTimeField(fieldId, field);
        column.CatalogReadOnly = field != nullptr && field->ReadOnly;
        column.NeedsAllowEditsCheck = RequiresAllowEditsCheck(column.Plan);
        allColumns.push_back(column);
    }

    std::unordered_map<std::string, TicketGridColumn> byKey;
    for (const auto& col : allColumns) {
        byKey[col.Key] = col;
    }

    std::unordered_set<std::string> usedKeys;
    for (const auto& key : view.ColumnOrder) {
        const auto it = byKey.find(key);
        if (it == byKey.end()) {
            continue;
        }
        columns.push_back(it->second);
        usedKeys.insert(key);
    }
    std::copy_if(allColumns.begin(), allColumns.end(), std::back_inserter(columns),
                 [&](const TicketGridColumn& col) { return usedKeys.find(col.Key) == usedKeys.end(); });

    return columns;
}
