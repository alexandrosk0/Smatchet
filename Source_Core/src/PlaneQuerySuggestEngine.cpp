#include "PlaneQuerySuggestEngine.h"

#include "AppController.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

static bool IsPlaneIdChar(unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '.' || ch == '-'; }

static bool ValueNeedsQuotes(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    for (unsigned char ch : s) {
        if (!IsPlaneIdChar(ch) || ch == '"' || ch == '\\') {
            return true;
        }
    }
    return false;
}

static std::string QuotedValue(const std::string& s) {
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

static std::string InsertForValueToken(const std::string& raw) {
    if (ValueNeedsQuotes(raw)) {
        return QuotedValue(raw);
    }
    return raw;
}

static void ScanStringStateToCursor(const char* buf, int bufLen, int cursor, bool& outInString, int& outStringOpenIndex) {
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

static bool AsciiEqualsIgnoreCaseToLowered(const std::string& value, const std::string& alreadyLowered) {
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

static const TrackerField* FindFieldToken(const AppController& app, const std::string& token) {
    if (token.empty()) {
        return nullptr;
    }
    const std::string key = ToLowerAsciiCopy(token);
    for (const auto& f : app.GetAvailableFields()) {
        if (AsciiEqualsIgnoreCaseToLowered(f.Id, key)) {
            return &f;
        }
    }
    for (const auto& f : app.GetAvailableFields()) {
        if (!f.Name.empty() && AsciiEqualsIgnoreCaseToLowered(f.Name, key)) {
            return &f;
        }
    }
    return nullptr;
}

static void AddUnique(std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen, std::string label,
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

static bool AsciiStartsWithIgnoreCase(const std::string& value, const std::string& prefixLower) {
    if (prefixLower.empty()) {
        return true;
    }
    const std::string v = ToLowerAsciiCopy(value);
    return v.size() >= prefixLower.size() && v.compare(0, prefixLower.size(), prefixLower) == 0;
}

static void AppendTerms(const std::string& prefix, const char* const* terms, int termCount, std::vector<QuerySuggestion>& out,
                        std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    for (int i = 0; i < termCount; ++i) {
        const std::string ins(terms[i]);
        if (AsciiStartsWithIgnoreCase(ins, pre)) {
            AddUnique(out, seen, ins, ins);
        }
    }
}

static void AppendFieldCatalog(const AppController& app, const std::string& prefix, std::vector<QuerySuggestion>& out,
                               std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    for (const auto& f : app.GetAvailableFields()) {
        if (f.Id.empty()) {
            continue;
        }
        if (AsciiStartsWithIgnoreCase(f.Id, pre)) {
            std::string label = f.Name.empty() ? f.Id : (f.Name + " (" + f.Id + ")");
            AddUnique(out, seen, std::move(label), f.Id);
        }
        if (!f.Name.empty() && f.Name != f.Id) {
            if (AsciiStartsWithIgnoreCase(f.Name, pre)) {
                std::string label = f.Name + " (" + f.Id + ")";
                AddUnique(out, seen, label, f.Id);
            }
        }
    }
}

static bool IsUserField(const TrackerField& field) {
    return field.IsUserType || field.Family == TrackerFieldFamily::UserSingle ||
           field.Family == TrackerFieldFamily::UserMulti;
}

static void AppendValueSuggestions(const TrackerField& field, const std::string& prefix, std::vector<QuerySuggestion>& out,
                                   std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    const bool isUserField = IsUserField(field);
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
        AddUnique(out, seen, std::move(label), insert);
    };

    for (const auto& opt : field.AllowedValueOptions) {
        if (isUserField) {
            const std::string display = opt.Value.empty() ? opt.SecondaryValue : opt.Value;
            const std::string accountId = opt.Id;
            if (!accountId.empty() && matchesPrefix(accountId, display)) {
                AddUnique(out, seen, display.empty() ? accountId : display, InsertForValueToken(accountId));
            }
            if (!display.empty() && display != accountId && matchesPrefix(display, display)) {
                AddUnique(out, seen, display + " (display) -> " + InsertForValueToken(display), InsertForValueToken(display));
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
static bool ParsePlaneValueContext(const char* buf, int /*bufLen*/, int replaceStart, const AppController& app,
                                   const TrackerField** outField) {
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
    while (startField > 0 && IsPlaneIdChar(static_cast<unsigned char>(buf[startField - 1]))) {
        --startField;
    }
    const std::string fieldTok(buf + startField, buf + endField + 1);
    *outField = FindFieldToken(app, fieldTok);
    return *outField != nullptr;
}

} // namespace

void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd, const AppController& app,
                                QuerySuggestBuild& out, QuerySuggestMeta* metaOut) {
    if (metaOut != nullptr) {
        metaOut->UserValueToken = false;
        metaOut->UserSearchPrefix.clear();
    }
    out.Items.clear();
    out.ReplaceStart = 0;
    out.ReplaceEnd = 0;
    std::unordered_set<std::string> seen;
    if (buf == nullptr) {
        return;
    }
    cursor = (std::max)(0, (std::min)(cursor, bufLen));
    selStart = (std::max)(0, (std::min)(selStart, bufLen));
    selEnd = (std::max)(0, (std::min)(selEnd, bufLen));

    int replaceStart = 0;
    int replaceEnd = 0;
    std::string prefix;

    if (selStart != selEnd) {
        const int lo = (std::min)(selStart, selEnd);
        const int hi = (std::max)(selStart, selEnd);
        replaceStart = lo;
        replaceEnd = hi;
        prefix.assign(buf + lo, buf + hi);
    } else {
        bool inString = false;
        int stringOpen = -1;
        ScanStringStateToCursor(buf, bufLen, cursor, inString, stringOpen);
        if (inString && stringOpen >= 0 && cursor > stringOpen + 1) {
            replaceStart = stringOpen + 1;
            replaceEnd = cursor;
            prefix.assign(buf + replaceStart, buf + replaceEnd);
        } else {
            int L = cursor;
            int R = cursor;
            while (L > 0 && IsPlaneIdChar(static_cast<unsigned char>(buf[L - 1]))) {
                --L;
            }
            while (R < bufLen && IsPlaneIdChar(static_cast<unsigned char>(buf[R]))) {
                ++R;
            }
            replaceStart = L;
            replaceEnd = R;
            prefix.assign(buf + L, buf + R);
        }
    }

    out.ReplaceStart = replaceStart;
    out.ReplaceEnd = replaceEnd;

    const TrackerField* valueField = nullptr;
    static const char* kLogical[] = {"AND", "OR"};
    if (ParsePlaneValueContext(buf, bufLen, replaceStart, app, &valueField)) {
        if (valueField != nullptr &&
            (!valueField->AllowedValueOptions.empty() || !valueField->AllowedValues.empty())) {
            AppendValueSuggestions(*valueField, prefix, out.Items, seen);
        }
        if (metaOut != nullptr && valueField != nullptr && IsUserField(*valueField)) {
            metaOut->UserValueToken = true;
            metaOut->UserSearchPrefix = prefix;
        }
    } else {
        if (!prefix.empty()) {
            AppendTerms(prefix, kLogical, static_cast<int>(sizeof(kLogical) / sizeof(kLogical[0])), out.Items, seen);
        }
        AppendFieldCatalog(app, prefix, out.Items, seen);
    }

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
    std::sort(out.Items.begin(), out.Items.end(), labelLessAscii);
    constexpr int kMaxSuggestions = 80;
    if (static_cast<int>(out.Items.size()) > kMaxSuggestions) {
        out.Items.resize(static_cast<size_t>(kMaxSuggestions));
    }
}
