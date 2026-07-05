#include "Tracker/PlaneQuerySuggestEnginePure.h"

#include "StringUtil.h"
#include "Tracker/TrackerQuerySuggestCommon.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=shared-helper using-block, extraction artefact; owner=tracker-backend; revisit=2026-12-31)
// clang-format on
using tracker_query_suggest::AddSuggestionUnique;
using tracker_query_suggest::AppendFieldCatalog;
using tracker_query_suggest::AppendTerms;
using tracker_query_suggest::AsciiStartsWithIgnoreCase;
using tracker_query_suggest::FindTrackerField;
using tracker_query_suggest::InsertForValueToken;
using tracker_query_suggest::IsQueryIdChar;
using tracker_query_suggest::IsQueryUserField;
using tracker_query_suggest::ScanStringStateToCursor;

// Near-twin of Jira's AppendValueSuggestions, kept per-engine on purpose: Jira labels user options
// " (display name)", Plane " (display)" — folding would collapse a genuine backend-local label divergence. The clone
// only surfaced after the cluster-A helper-name unification removed the cosmetic-identifier difference the gate keyed
// on.
static void AppendValueSuggestions(const TrackerField& field, const std::string& prefix,
                                   std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen) {
    // SMATCHET_DEVIATION(rule=duplication; reason=per-engine near-twin; owner=tracker-backend; revisit=2026-12-31)
    const std::string pre = ToLowerAsciiCopy(prefix);
    const bool isUserField = IsQueryUserField(field);
    auto matchesPrefix = [&](const std::string& raw, const std::string& label) {
        return AsciiStartsWithIgnoreCase(raw, pre) || AsciiStartsWithIgnoreCase(label, pre);
    };
    auto tryAdd = [&](const std::string& raw, const std::string& displayLabel) {
        if (raw.empty()) {
            return;
        }
        if (!matchesPrefix(raw, displayLabel)) {
            return;
        }
        const std::string insert = InsertForValueToken(raw);
        std::string label = displayLabel.empty() ? raw : displayLabel;
        if (label != insert && !insert.empty() && insert.front() == '"') {
            label = label + " -> " + insert;
        }
        AddSuggestionUnique(out, seen, std::move(label), insert);
    };

    for (const auto& opt : field.AllowedValueOptions) {
        if (isUserField) {
            const std::string display = opt.Value.empty() ? opt.SecondaryValue : opt.Value;
            const std::string accountId = opt.Id;
            if (!accountId.empty() && matchesPrefix(accountId, display)) {
                AddSuggestionUnique(out, seen, display.empty() ? accountId : display, InsertForValueToken(accountId));
            }
            if (!display.empty() && display != accountId && matchesPrefix(display, display)) {
                AddSuggestionUnique(out, seen, display + " (display) -> " + InsertForValueToken(display),
                                    InsertForValueToken(display));
            }
            continue;
        }
        if (!opt.Value.empty()) {
            tryAdd(opt.Value, opt.Value);
        }
        if (!opt.Id.empty() && opt.Id != opt.Value) {
            tryAdd(opt.Id, opt.Id + " (" + opt.Value + ")");
        }
    }
    for (const auto& v : field.AllowedValues) {
        tryAdd(v, v);
    }
}

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

namespace PlaneQuerySuggestEnginePure {

void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                const std::vector<TrackerField>& fields, QuerySuggestBuild& out,
                                QuerySuggestMeta* metaOut) {
    if (!tracker_query_suggest::BeginSuggestBuild(buf, bufLen, cursor, selStart, selEnd, out, metaOut)) {
        return;
    }
    std::unordered_set<std::string> seen;

    const std::string prefix =
        tracker_query_suggest::ResolveQueryReplaceRange(buf, bufLen, cursor, selStart, selEnd, out);

    const TrackerField* valueField = nullptr;
    if (ParsePlaneValueContext(buf, bufLen, out.ReplaceStart, fields, &valueField)) {
        if (valueField != nullptr && (!valueField->AllowedValueOptions.empty() || !valueField->AllowedValues.empty())) {
            AppendValueSuggestions(*valueField, prefix, out.Items, seen);
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

    tracker_query_suggest::SortAndCapSuggestions(out.Items);
}

} // namespace PlaneQuerySuggestEnginePure
