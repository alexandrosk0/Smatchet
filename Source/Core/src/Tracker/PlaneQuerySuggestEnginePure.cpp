#include "PlaneQuerySuggestEnginePure.h"

#include "Tracker/TrackerQuerySuggestCommon.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

// SMATCHET_DEVIATION(rule=duplication; reason=shared-helper using-block; owner=tracker-backend; revisit=2026-12-31)
using tracker_query_suggest::AppendFieldCatalog;
using tracker_query_suggest::AppendTerms;
using tracker_query_suggest::AppendValueSuggestions;
using tracker_query_suggest::BeginQuerySuggestPass;
using tracker_query_suggest::FindTrackerField;
using tracker_query_suggest::IsQueryIdChar;
using tracker_query_suggest::IsQueryUserField;
using tracker_query_suggest::SortAndCapQuerySuggestions;

// Plane's wording for the display-name variant of a user-field value suggestion. The sole
// backend-local divergence in the otherwise shared AppendValueSuggestions body — Jira says
// " (display name) -> ". Kept verbatim; only the body is now single-sourced.
constexpr const char* kPlaneUserDisplaySuffix = " (display) -> ";

/** If cursor sits in value token after `field:` or `field=`, set field and return true. */
static bool ParsePlaneValueContext(const char* buf, int /*bufLen*/, int replaceStart,
                                   const std::vector<TrackerField>& fields, const TrackerField** outField) {
    *outField = nullptr;
    if (replaceStart <= 0 || buf == nullptr) {
        return false;
    }
    int p = replaceStart - 1;
    while (p >= 0 && std::isspace(static_cast<unsigned char>(buf[p])) != 0) {
        --p;
    }
    if (p < 0 || (buf[p] != ':' && buf[p] != '=')) {
        return false;
    }
    --p;
    while (p >= 0 && std::isspace(static_cast<unsigned char>(buf[p])) != 0) {
        --p;
    }
    if (p < 0) {
        return false;
    }
    int endField = p;
    int startField = endField;
    while (startField > 0 && IsQueryIdChar(static_cast<unsigned char>(buf[startField - 1]))) {
        --startField;
    }
    const std::string fieldTok(buf + startField, buf + endField + 1);
    *outField = FindTrackerField(fields, fieldTok);
    return *outField != nullptr;
}

} // namespace

void BuildPlaneQuerySuggestionsPure(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                    const std::vector<TrackerField>& fields, QuerySuggestBuild& out,
                                    QuerySuggestMeta* metaOut) {
    // SMATCHET_DEVIATION(rule=duplication; reason=engine entry scaffolding; owner=tracker-backend; revisit=2026-12-31)
    std::unordered_set<std::string> seen;
    int replaceStart = 0;
    int replaceEnd = 0;
    std::string prefix;
    if (!BeginQuerySuggestPass(buf, bufLen, cursor, selStart, selEnd, out, metaOut, replaceStart, replaceEnd, prefix)) {
        return;
    }

    const TrackerField* valueField = nullptr;
    if (ParsePlaneValueContext(buf, bufLen, replaceStart, fields, &valueField)) {
        if (valueField != nullptr && (!valueField->AllowedValueOptions.empty() || !valueField->AllowedValues.empty())) {
            AppendValueSuggestions(*valueField, prefix, kPlaneUserDisplaySuffix, out.Items, seen);
        }
        if (metaOut != nullptr && valueField != nullptr && IsQueryUserField(*valueField)) {
            metaOut->UserValueToken = true;
            metaOut->UserSearchPrefix = prefix;
        }
    } else {
        if (!prefix.empty()) {
            static const char* kLogical[] = {"AND", "OR"};
            AppendTerms(prefix, kLogical, static_cast<int>(sizeof(kLogical) / sizeof(kLogical[0])), out.Items, seen);
        }
        AppendFieldCatalog(fields, prefix, out.Items, seen);
    }

    SortAndCapQuerySuggestions(out.Items);
}