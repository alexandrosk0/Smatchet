#include "PlaneQuerySuggestEnginePure.h"

#include "Tracker/TrackerQuerySuggestCommon.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace {

using tracker_query_suggest::AppendFieldCatalog;
using tracker_query_suggest::AppendTerms;
using tracker_query_suggest::AppendValueSuggestions;
using tracker_query_suggest::BeginQuerySuggestPass;
using tracker_query_suggest::FindTrackerField;
using tracker_query_suggest::IsQueryIdChar;
using tracker_query_suggest::IsQueryUserField;
using tracker_query_suggest::QuerySuggestPass;
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
    QuerySuggestPass pass;
    if (!BeginQuerySuggestPass(buf, bufLen, cursor, selStart, selEnd, out, metaOut, pass)) {
        return;
    }

    const TrackerField* valueField = nullptr;
    if (ParsePlaneValueContext(buf, bufLen, pass.ReplaceStart, fields, &valueField)) {
        if (valueField != nullptr && (!valueField->AllowedValueOptions.empty() || !valueField->AllowedValues.empty())) {
            AppendValueSuggestions(*valueField, pass.Prefix, kPlaneUserDisplaySuffix, out.Items, pass.Seen);
        }
        if (metaOut != nullptr && valueField != nullptr && IsQueryUserField(*valueField)) {
            metaOut->UserValueToken = true;
            metaOut->UserSearchPrefix = pass.Prefix;
        }
    } else {
        if (!pass.Prefix.empty()) {
            static const char* kLogical[] = {"AND", "OR"};
            AppendTerms(pass.Prefix, kLogical, static_cast<int>(sizeof(kLogical) / sizeof(kLogical[0])), out.Items,
                        pass.Seen);
        }
        AppendFieldCatalog(fields, pass.Prefix, out.Items, pass.Seen);
    }

    SortAndCapQuerySuggestions(out.Items);
}