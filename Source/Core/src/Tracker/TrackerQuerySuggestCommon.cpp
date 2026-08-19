#include "Tracker/TrackerQuerySuggestCommon.h"

#include "StringUtil.h"

#include <algorithm>
#include <cctype>

namespace tracker_query_suggest {

bool IsQueryIdChar(unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '.' || ch == '-'; }

bool QueryValueNeedsQuotes(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    return std::any_of(s.begin(), s.end(), [](char ch) {
        return !IsQueryIdChar(static_cast<unsigned char>(ch)) || ch == '"' || ch == '\\';
    });
}

std::string QueryQuotedValue(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
    return out;
}

std::string InsertForValueToken(const std::string& raw) {
    if (QueryValueNeedsQuotes(raw)) {
        return QueryQuotedValue(raw);
    }
    return raw;
}

void ScanStringStateToCursor(const char* buf, int bufLen, int cursor, bool& outInString, int& outStringOpenIndex) {
    outInString = false;
    outStringOpenIndex = -1;
    const int lim = (std::min)(bufLen, cursor);
    for (int j = 0; j < lim; ++j) {
        const unsigned char ch = static_cast<unsigned char>(buf[j]);
        if (ch == '\\' && outInString && j + 1 < lim) {
            ++j;
            continue;
        }
        if (ch == '"') {
            if (!outInString) {
                outStringOpenIndex = j;
                outInString = true;
            } else {
                outInString = false;
                outStringOpenIndex = -1;
            }
        }
    }
}

bool AsciiEqualsIgnoreCaseToLowered(const std::string& value, const std::string& alreadyLowered) {
    if (value.size() != alreadyLowered.size()) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(value[i]))) != alreadyLowered[i]) {
            return false;
        }
    }
    return true;
}

bool AsciiStartsWithIgnoreCase(const std::string& value, const std::string& prefixLower) {
    if (prefixLower.empty()) {
        return true;
    }
    const std::string v = ToLowerAsciiCopy(value);
    return v.size() >= prefixLower.size() && v.compare(0, prefixLower.size(), prefixLower) == 0;
}

bool AsciiContainsIgnoreCase(const std::string& value, const std::string& needleLower) {
    if (needleLower.empty()) {
        return true;
    }
    const std::string v = ToLowerAsciiCopy(value);
    return v.find(needleLower) != std::string::npos;
}

const TrackerField* FindTrackerField(const std::vector<TrackerField>& fields, const std::string& token) {
    if (token.empty()) {
        return nullptr;
    }
    const std::string key = ToLowerAsciiCopy(token);
    auto idIt = std::find_if(fields.begin(), fields.end(),
                             [&](const TrackerField& f) { return AsciiEqualsIgnoreCaseToLowered(f.Id, key); });
    if (idIt != fields.end()) {
        return &(*idIt);
    }
    auto nameIt = std::find_if(fields.begin(), fields.end(), [&](const TrackerField& f) {
        return !f.Name.empty() && AsciiEqualsIgnoreCaseToLowered(f.Name, key);
    });
    if (nameIt != fields.end()) {
        return &(*nameIt);
    }
    return nullptr;
}

void AddSuggestionUnique(std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen, std::string label,
                         std::string insert) {
    if (insert.empty()) {
        return;
    }
    if (!seen.insert(insert).second) {
        return;
    }
    if (label.empty()) {
        label = insert;
    }
    out.push_back(QuerySuggestion{std::move(label), std::move(insert)});
}

void AppendTerms(const std::string& prefix, const char* const* terms, int termCount, std::vector<QuerySuggestion>& out,
                 std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    for (int i = 0; i < termCount; ++i) {
        const std::string ins(terms[i]);
        if (AsciiStartsWithIgnoreCase(ins, pre)) {
            AddSuggestionUnique(out, seen, ins, ins);
        }
    }
}

void AppendFieldCatalog(const std::vector<TrackerField>& fields, const std::string& prefix,
                        std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    for (const auto& f : fields) {
        if (f.Id.empty()) {
            continue;
        }
        if (AsciiStartsWithIgnoreCase(f.Id, pre)) {
            std::string label = f.Name.empty() ? f.Id : (f.Name + " (" + f.Id + ")");
            AddSuggestionUnique(out, seen, std::move(label), f.Id);
        }
        if (!f.Name.empty() && f.Name != f.Id) {
            if (AsciiStartsWithIgnoreCase(f.Name, pre)) {
                std::string label = f.Name + " (" + f.Id + ")";
                AddSuggestionUnique(out, seen, label, f.Id);
            }
        }
    }
}

bool IsQueryUserField(const TrackerField& field) {
    return field.IsUserType || field.Family == TrackerFieldFamily::UserSingle ||
           field.Family == TrackerFieldFamily::UserMulti;
}

bool IsQueryDateField(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::Date || field.Family == TrackerFieldFamily::DateTime;
}

void AppendValueSuggestions(const TrackerField& field, const std::string& prefix, const char* userDisplaySuffix,
                            std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    const bool isUserField = IsQueryUserField(field);
    // A user field's DISPLAY text matches anywhere (a surname is as natural a search key as
    // a first name), but its raw side stays prefix-anchored: for user fields the raw value
    // is the opaque 24-hex accountId (TrackerFieldCatalog fills one option per catalog
    // user), and a substring test there matches most of the org on a single typed letter.
    // Every other value family stays fully prefix-anchored — there the typed text is the
    // start of a status / version / label the user already has in mind.
    auto matchesQuery = [&](const std::string& raw, const std::string& label) {
        if (isUserField) {
            return AsciiStartsWithIgnoreCase(raw, pre) || AsciiContainsIgnoreCase(label, pre);
        }
        return AsciiStartsWithIgnoreCase(raw, pre) || AsciiStartsWithIgnoreCase(label, pre);
    };
    auto tryAdd = [&](const std::string& raw, const std::string& displayLabel) {
        if (raw.empty()) {
            return;
        }
        if (!matchesQuery(raw, displayLabel)) {
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
            if (!accountId.empty() && matchesQuery(accountId, display)) {
                AddSuggestionUnique(out, seen, display.empty() ? accountId : display, InsertForValueToken(accountId));
            }
            if (!display.empty() && display != accountId && matchesQuery(display, display)) {
                AddSuggestionUnique(out, seen, display + userDisplaySuffix + InsertForValueToken(display),
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

void ResolveQueryReplaceRange(const char* buf, int bufLen, int cursor, int selStart, int selEnd, int& replaceStart,
                              int& replaceEnd, std::string& prefix) {
    if (selStart != selEnd) {
        const int lo = (std::min)(selStart, selEnd);
        const int hi = (std::max)(selStart, selEnd);
        replaceStart = lo;
        replaceEnd = hi;
        prefix.assign(buf + lo, buf + hi);
        return;
    }
    bool inString = false;
    int stringOpen = -1;
    ScanStringStateToCursor(buf, bufLen, cursor, inString, stringOpen);
    if (inString && stringOpen >= 0 && cursor > stringOpen + 1) {
        replaceStart = stringOpen + 1;
        replaceEnd = cursor;
        prefix.assign(buf + replaceStart, buf + replaceEnd);
        return;
    }
    int L = cursor;
    int R = cursor;
    while (L > 0 && IsQueryIdChar(static_cast<unsigned char>(buf[L - 1]))) {
        --L;
    }
    while (R < bufLen && IsQueryIdChar(static_cast<unsigned char>(buf[R]))) {
        ++R;
    }
    replaceStart = L;
    replaceEnd = R;
    prefix.assign(buf + L, buf + R);
}

bool BeginQuerySuggestPass(const char* buf, int bufLen, int cursor, int selStart, int selEnd, QuerySuggestBuild& out,
                           QuerySuggestMeta* metaOut, int& replaceStart, int& replaceEnd, std::string& prefix) {
    if (metaOut != nullptr) {
        metaOut->UserValueToken = false;
        metaOut->UserSearchPrefix.clear();
    }
    out.Items.clear();
    out.ReplaceStart = 0;
    out.ReplaceEnd = 0;
    if (buf == nullptr) {
        return false;
    }
    cursor = (std::max)(0, (std::min)(cursor, bufLen));
    selStart = (std::max)(0, (std::min)(selStart, bufLen));
    selEnd = (std::max)(0, (std::min)(selEnd, bufLen));

    ResolveQueryReplaceRange(buf, bufLen, cursor, selStart, selEnd, replaceStart, replaceEnd, prefix);
    out.ReplaceStart = replaceStart;
    out.ReplaceEnd = replaceEnd;
    return true;
}

void SortAndCapQuerySuggestions(std::vector<QuerySuggestion>& items) {
    auto labelLessAscii = [](const QuerySuggestion& a, const QuerySuggestion& b) {
        size_t i = 0;
        const size_t na = a.Label.size();
        const size_t nb = b.Label.size();
        for (; i < na && i < nb; ++i) {
            const int ca = std::tolower(static_cast<unsigned char>(a.Label[i]));
            const int cb = std::tolower(static_cast<unsigned char>(b.Label[i]));
            if (ca != cb) {
                return ca < cb;
            }
        }
        return na < nb;
    };
    std::sort(items.begin(), items.end(), labelLessAscii);
    constexpr int kMaxSuggestions = 80;
    if (static_cast<int>(items.size()) > kMaxSuggestions) {
        items.resize(static_cast<size_t>(kMaxSuggestions));
    }
}

} // namespace tracker_query_suggest
