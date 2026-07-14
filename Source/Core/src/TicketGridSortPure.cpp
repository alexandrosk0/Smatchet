// Pure sort + field-classification helpers — see TicketGridSortPure.h for the contract.
// Moved verbatim out of TicketGridModel.cpp (which stays home to the ImGui-coupled
// render-plan / column-builder code); behaviour is unchanged.

#include "TicketGridSortPure.h"

#include "StringUtil.h"
#include "TicketGridDurationSortPure.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unordered_set>

namespace {

const std::unordered_set<std::string> kTimeTrackingFieldIds = {
    "timeoriginalestimate",          "timeestimate",          "timespent",
    "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent"};

const std::unordered_set<std::string> kDateFieldIds = {"created", "updated", "duedate"};

} // namespace

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
        const unsigned char cx = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(x[i])));
        const unsigned char cy = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(y[i])));
        if (cx != cy) {
            return (cx < cy) ? -1 : 1;
        }
    }
    if (x.size() != y.size()) {
        return (x.size() < y.size()) ? -1 : 1;
    }
    return 0;
}

int CompareIssueKeyValues(const std::string& aVal, const std::string& bVal) {
    if (CompareIssueKeyNatural(aVal, bVal)) {
        return -1;
    }
    if (CompareIssueKeyNatural(bVal, aVal)) {
        return 1;
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
        return TicketGridDurationSortPure::CompareTimeTrackingValues(aVal, bVal);
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
