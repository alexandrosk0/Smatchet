// Pure sort + field-classification helpers — see TicketGridSortPure.h for the contract.
// Moved verbatim out of TicketGridModel.cpp (which stays home to the ImGui-coupled
// render-plan / column-builder code); behaviour is unchanged.

#include "TicketGridSortPure.h"

#include "StringUtil.h"
#include "TicketGridDurationSortPure.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
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
    // strtod whole-string-matches "nan" / "inf". NaN compares false against everything,
    // including itself, so a NaN cell makes the comparator return +1 for both Compare(a,b)
    // and Compare(b,a) — not a strict weak ordering, which is UB in stable_sort. So NaN
    // must not reach the numeric path; it falls to the text class instead, where it is
    // just the three-letter string it looks like.
    //
    // Reject NaN ONLY, never ±inf: infinities compare consistently as doubles, so they
    // belong in the numeric class where a user reading a numeric column expects them
    // (sorted at the ends, not filed alphabetically between "-1" and "0").
    if (std::isnan(out)) {
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

/** Default compare for a column with no type-specific handler: numeric-aware, but with
 *  "does it parse as a number" as the PRIMARY key. `useDouble` picks the double parse
 *  (number-typed fields) vs the int64 parse (everything else).
 *
 *  Key, in order: (parses ? 0 : 1, numeric value, raw string). Both parse → numeric
 *  compare; only one parses → the number sorts first (ascending); neither → case-
 *  insensitive string compare, which also breaks a numeric tie so "1", "1.0" and "01"
 *  stay ordered against each other.
 *
 *  Category-first is what makes this a strict weak ordering — required, since
 *  stable_sort with a non-SWO comparator is UB. Comparing SOME pairs of a mixed column
 *  numerically and others lexically is intransitive: with the old "both parse or fall
 *  through" shape, "5x" < "9" (text) and "9" < "10" (numeric) but "10" < "5x" (text). */
int CompareValuesNumericThenText(const std::string& aVal, const std::string& bVal, bool useDouble) {
    bool aNum = false;
    bool bNum = false;
    int numericCmp = 0;
    if (useDouble) {
        double da = 0;
        double db = 0;
        aNum = ParseWholeDouble(aVal, da);
        bNum = ParseWholeDouble(bVal, db);
        if (aNum && bNum) {
            // Ordered via < in both directions rather than != — no float equality compare.
            numericCmp = (da < db) ? -1 : ((db < da) ? 1 : 0);
        }
    } else {
        long long na = 0;
        long long nb = 0;
        aNum = ParseWholeInt64Dec(aVal, na);
        bNum = ParseWholeInt64Dec(bVal, nb);
        if (aNum && bNum && na != nb) {
            numericCmp = (na < nb) ? -1 : 1;
        }
    }
    if (aNum != bNum) {
        return aNum ? -1 : 1;
    }
    if (numericCmp != 0) {
        return numericCmp;
    }
    return CompareCaseInsensitive(aVal, bVal);
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
    return CompareValuesNumericThenText(aVal, bVal, useDouble);
}

bool IsTrackerDateOrDateTimeField(const std::string& fieldId, const TrackerField* field) {
    // Duration columns hold raw seconds, not dates — check them first (same ordering
    // CompareFieldValuesForSort uses) so the "time" word-heuristic below can't claim them.
    if (kTimeTrackingFieldIds.count(fieldId)) {
        return false;
    }
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
