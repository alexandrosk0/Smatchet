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
#include <limits>
#include <unordered_set>

namespace {

constexpr long long kSecondsPerHour = 3600;
constexpr long long kSecondsPerDay = 8 * kSecondsPerHour;
constexpr long long kSecondsPerWeek = 5 * kSecondsPerDay;
// Caps a hostile/garbled field value (e.g. "99999999999999w") from overflowing `long long`
// (UB) in the multiply/add below — this is a sort comparator, so an out-of-range value just
// needs to saturate to "very large", not be exact. CPP_CODE_AUDIT.md #19.
constexpr long long kMaxDurationSeconds = (std::numeric_limits<long long>::max)() / 2;

void SaturatingAccumulateDuration(long long& total, long long num, long long perUnit) {
    if (num > 0 && perUnit > 0 && num > kMaxDurationSeconds / perUnit) {
        total = kMaxDurationSeconds;
        return;
    }
    const long long added = num * perUnit;
    if (total > kMaxDurationSeconds - added) {
        total = kMaxDurationSeconds;
    } else {
        total += added;
    }
}

std::string TrimSpacesTabs(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        --b;
    }
    return s.substr(a, b - a);
}

// Whole-string integer fast path ("3600" == plain seconds). Returns true + sets outValue when
// the entire (trimmed) string parses as one integer; false to fall through to the manual
// unit-by-unit parse below (e.g. "3h 30m").
bool TryParseWholeDurationSeconds(const std::string& s, long long& outValue) {
    size_t pos = 0;
    try {
        long long v = std::stoll(s, &pos, 10);
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
            ++pos;
        }
        if (pos >= s.size()) {
            outValue = v;
            return true;
        }
    } catch (...) {
        // catch-all-ok: std::stoll throws on non-numeric input — intentional fall-through to the
        // manual unit-by-unit duration parse below.
    }
    return false;
}

// Manual unit-by-unit duration parse ("3h 30m", "2.5h", ...) for values the whole-string fast
// path rejected.
long long ParseDurationUnitsSum(const std::string& s) {
    size_t pos = 0;
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
            SaturatingAccumulateDuration(total, num, 1);
            break;
        }
        const char u = s[pos];
        if (u == 'w' || u == 'W') {
            SaturatingAccumulateDuration(total, num, kSecondsPerWeek);
            ++pos;
        } else if (u == 'd' || u == 'D') {
            SaturatingAccumulateDuration(total, num, kSecondsPerDay);
            ++pos;
        } else if (u == 'h' || u == 'H') {
            SaturatingAccumulateDuration(total, num, kSecondsPerHour);
            ++pos;
        } else if (u == 'm' || u == 'M') {
            SaturatingAccumulateDuration(total, num, 60LL);
            ++pos;
        } else {
            // Non-unit char (e.g. the '.' in "2.5h") — not a recognized unit suffix. Advance
            // past it so the loop always makes progress; without this `pos` never moves and
            // `num` gets re-added forever (infinite loop / permanent UI freeze on sort —
            // CPP_CODE_AUDIT.md #3).
            SaturatingAccumulateDuration(total, num, 1);
            ++pos;
        }
    }
    return total;
}

long long ParseDurationToSecondsForSort(const std::string& input) {
    const std::string s = TrimSpacesTabs(input);
    if (s.empty()) {
        return 0;
    }
    long long wholeValue = 0;
    if (TryParseWholeDurationSeconds(s, wholeValue)) {
        return wholeValue;
    }
    return ParseDurationUnitsSum(s);
}

const std::unordered_set<std::string> kTimeTrackingFieldIds = {
    "timeoriginalestimate",          "timeestimate",          "timespent",
    "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent"};

const std::unordered_set<std::string> kDateFieldIds = {"created", "updated", "duedate"};

// Returns a Special* render plan for id-only columns (field == null) by column-id
// predicate, or PlainText when nothing special matches.
TicketGridColumn::RenderPlan ResolveRenderPlanForFieldId(const std::string& fieldId) {
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

// Special* render plans that apply to a resolved field by id/metadata predicate, before
// edit-affordance dispatch. Returns true (with `out` set) when a special plan matches.
bool TryResolveSpecialFieldPlan(const TrackerField* field, TicketGridColumn::RenderPlan& out) {
    if (IsAttachmentFieldId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialAttachment;
        return true;
    }
    if (TrackerGridFieldDisplay::IsWatchersColumnId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialWatchers;
        return true;
    }
    if (TrackerGridFieldDisplay::IsVotesColumnId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialVotes;
        return true;
    }
    if (TrackerGridFieldDisplay::IsWorklogColumnId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialWorklog;
        return true;
    }
    if (TrackerGridFieldDisplay::IsProgressDisplayField(field)) {
        out = TicketGridColumn::RenderPlan::SpecialProgress;
        return true;
    }
    if (TrackerGridFieldDisplay::IsIssueRestrictionField(field)) {
        out = TicketGridColumn::RenderPlan::SpecialIssueRestriction;
        return true;
    }
    return false;
}

// Edit-affordance dispatch for a resolved, editable (non-special, non-read-only) field.
TicketGridColumn::RenderPlan ResolveEditableFieldPlan(const TrackerField* field) {
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

TicketGridColumn::RenderPlan ResolveRenderPlan(const std::string& fieldId, const TrackerField* field) {
    if (fieldId == "timespent" || (field && field->Id == "timespent")) {
        return TicketGridColumn::RenderPlan::SpecialTimeSpent;
    }
    if (field == nullptr) {
        return ResolveRenderPlanForFieldId(fieldId);
    }

    TicketGridColumn::RenderPlan special = TicketGridColumn::RenderPlan::PlainText;
    if (TryResolveSpecialFieldPlan(field, special)) {
        return special;
    }
    if (field->ReadOnly) {
        return TicketGridColumn::RenderPlan::PlainText;
    }
    return ResolveEditableFieldPlan(field);
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

namespace {

/** Total-order case-insensitive string compare with length tie-break (-1 / 0 / +1). */
int CompareCaseInsensitive(const std::string& x, const std::string& y) {
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
}

/** Natural issue-key compare (key/issuekey fields). */
int CompareIssueKeyValues(const std::string& aVal, const std::string& bVal) {
    if (CompareIssueKeyNatural(aVal, bVal)) {
        return -1;
    }
    if (CompareIssueKeyNatural(bVal, aVal)) {
        return 1;
    }
    return 0;
}

/** Time-tracking duration compare (e.g. "2d 4h" -> seconds). */
int CompareTimeTrackingValues(const std::string& aVal, const std::string& bVal) {
    const long long sa = ParseDurationToSecondsForSort(aVal);
    const long long sb = ParseDurationToSecondsForSort(bVal);
    if (sa != sb) {
        return (sa < sb) ? -1 : 1;
    }
    return 0;
}

/** Date/datetime compare — lexical on the raw ISO string (sortable as-is). */
int CompareDateValues(const std::string& aVal, const std::string& bVal) {
    const int cmp = aVal.compare(bVal);
    if (cmp != 0) {
        return (cmp < 0) ? -1 : 1;
    }
    return 0;
}

/** Numeric compare. `useDouble` picks the double parse (number-typed fields) vs the
 *  int64 parse (everything else). On parse failure, `outNumeric` stays false and the
 *  caller falls back to case-insensitive string compare. */
int CompareNumericValues(const std::string& aVal, const std::string& bVal, bool useDouble, bool& outNumeric) {
    outNumeric = false;
    if (useDouble) {
        double da = 0;
        double db = 0;
        const bool aNum = ParseWholeDouble(aVal, da);
        const bool bNum = ParseWholeDouble(bVal, db);
        if (aNum && bNum) {
            outNumeric = true;
            if (da != db) {
                return (da < db) ? -1 : 1;
            }
            return 0;
        }
        return 0;
    }
    long long na = 0;
    long long nb = 0;
    const bool aInt = ParseWholeInt64Dec(aVal, na);
    const bool bInt = ParseWholeInt64Dec(bVal, nb);
    if (aInt && bInt) {
        outNumeric = true;
        if (na != nb) {
            return (na < nb) ? -1 : 1;
        }
        return 0;
    }
    return 0;
}

} // namespace

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
        return CompareIssueKeyValues(aVal, bVal);
    }
    if (kTimeTrackingFieldIds.count(fieldId)) {
        return CompareTimeTrackingValues(aVal, bVal);
    }
    if (kDateFieldIds.count(fieldId) || (fieldMeta && (fieldMeta->Type == "date" || fieldMeta->Type == "datetime"))) {
        return CompareDateValues(aVal, bVal);
    }

    const bool useDouble = fieldMeta && fieldMeta->Type == "number";
    bool wasNumeric = false;
    const int numericCmp = CompareNumericValues(aVal, bVal, useDouble, wasNumeric);
    if (wasNumeric) {
        return numericCmp;
    }
    return CompareCaseInsensitive(aVal, bVal);
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
        // issue-comments fix (#1291) — fold Jira's legacy `comment` column onto the unified `comments`
        // cell so a view saved before the dedupe renders the count/modal (not the raw ADF blob) and
        // dedups against an explicit `comments` column via seenFieldIds below. See CanonicalizeGridFieldId.
        const std::string fieldId = CanonicalizeGridFieldId(TrimCopyAsciiWhitespace(rawFieldId));
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
    for (const auto& rawKey : view.ColumnOrder) {
        // Canonicalize the saved order key the same way the column Key was built from view.Fields
        // above (ASCII-whitespace trim + legacy-alias fold, e.g. `field:comment` → `field:comments`,
        // #1291). A pre-canonicalization view — or one carrying stray whitespace — then keeps each
        // column's saved position instead of dropping it to the appended tail; the dedup guard drops
        // a ColumnOrder that lists the same canonical key twice (ticketgrid-columnorder-canon).
        const std::string key = CanonicalGridColumnKey(rawKey);
        const auto it = byKey.find(key);
        if (it == byKey.end() || !usedKeys.insert(key).second) {
            continue;
        }
        columns.push_back(it->second);
    }
    std::copy_if(allColumns.begin(), allColumns.end(), std::back_inserter(columns),
                 [&](const TicketGridColumn& col) { return usedKeys.find(col.Key) == usedKeys.end(); });

    return columns;
}
