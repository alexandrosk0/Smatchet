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

} // namespace tracker_query_suggest
