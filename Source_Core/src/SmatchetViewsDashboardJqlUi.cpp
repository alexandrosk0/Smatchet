#include "SmatchetViewsDashboardUi_detail.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
struct JqlSuggestion {
    std::string Label;
    std::string Insert;
};

struct JqlSuggestBuild {
    int ReplaceStart = 0;
    int ReplaceEnd = 0;
    std::vector<JqlSuggestion> Items;
};

static bool IsJqlIdChar(unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '.' || ch == '-'; }

static bool JqlValueNeedsQuotes(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    for (unsigned char ch : s) {
        if (!IsJqlIdChar(ch) || ch == '"' || ch == '\\') {
            return true;
        }
    }
    return false;
}

static std::string JqlQuotedValue(const std::string& s) {
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

static std::string JqlInsertForValueToken(const std::string& raw) {
    if (JqlValueNeedsQuotes(raw)) {
        return JqlQuotedValue(raw);
    }
    return raw;
}

static void JqlScanStringStateToCursor(const char* buf, int bufLen, int cursor, bool& outInString,
                                       int& outStringOpenIndex) {
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

static const TrackerField* FindTrackerFieldForJqlToken(const AppController& app, const std::string& token) {
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

static void AddSuggestionUnique(std::vector<JqlSuggestion>& out, std::unordered_set<std::string>& seenInserts,
                                std::string label, std::string insert) {
    if (insert.empty()) {
        return;
    }
    if (!seenInserts.insert(insert).second) {
        return;
    }
    if (label.empty()) {
        label = insert;
    }
    out.push_back(JqlSuggestion{std::move(label), std::move(insert)});
}

static bool AsciiStartsWithIgnoreCase(const std::string& value, const std::string& prefixLower) {
    if (prefixLower.empty()) {
        return true;
    }
    const std::string v = ToLowerAsciiCopy(value);
    return v.size() >= prefixLower.size() && v.compare(0, prefixLower.size(), prefixLower) == 0;
}

static void AppendJqlTerms(const std::string& prefix, const char* const* terms, int termCount,
                           std::vector<JqlSuggestion>& out, std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    for (int i = 0; i < termCount; ++i) {
        const std::string ins(terms[i]);
        if (AsciiStartsWithIgnoreCase(ins, pre)) {
            AddSuggestionUnique(out, seen, ins, ins);
        }
    }
}

static void AppendFieldCatalogSuggestions(const AppController& app, const std::string& prefix,
                                          std::vector<JqlSuggestion>& out, std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    for (const auto& f : app.GetAvailableFields()) {
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

static bool IsJqlUserField(const TrackerField& field) {
    return field.IsUserType || field.Family == TrackerFieldFamily::UserSingle ||
           field.Family == TrackerFieldFamily::UserMulti;
}

static void AppendValueSuggestions(const TrackerField& field, const std::string& prefix,
                                   std::vector<JqlSuggestion>& out, std::unordered_set<std::string>& seen) {
    const std::string pre = ToLowerAsciiCopy(prefix);
    const bool isUserField = IsJqlUserField(field);
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
        const std::string insert = JqlInsertForValueToken(raw);
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
                AddSuggestionUnique(out, seen, display.empty() ? accountId : display,
                                    JqlInsertForValueToken(accountId));
            }
            if (!display.empty() && display != accountId && matchesPrefix(display, display)) {
                AddSuggestionUnique(out, seen, display + " (display name) -> " + JqlInsertForValueToken(display),
                                    JqlInsertForValueToken(display));
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

struct JqlToken {
    std::string Text;
    char Punct = 0;
    bool Quoted = false;
};

static std::vector<JqlToken> TokenizeJqlPrefix(const char* buf, int end) {
    std::vector<JqlToken> tokens;
    int i = 0;
    while (i < end) {
        const unsigned char ch = static_cast<unsigned char>(buf[i]);
        if (std::isspace(ch) != 0) {
            ++i;
            continue;
        }
        if (ch == '"') {
            ++i;
            std::string text;
            while (i < end) {
                const unsigned char c = static_cast<unsigned char>(buf[i]);
                if (c == '\\' && i + 1 < end) {
                    text.push_back(buf[i + 1]);
                    i += 2;
                    continue;
                }
                if (c == '"') {
                    ++i;
                    break;
                }
                text.push_back(static_cast<char>(c));
                ++i;
            }
            tokens.push_back(JqlToken{std::move(text), 0, true});
            continue;
        }
        if (ch == '(' || ch == ')' || ch == ',') {
            tokens.push_back(JqlToken{std::string(1, static_cast<char>(ch)), static_cast<char>(ch), false});
            ++i;
            continue;
        }
        if (ch == '=' || ch == '!' || ch == '~' || ch == '>' || ch == '<') {
            std::string op(1, static_cast<char>(ch));
            if (i + 1 < end) {
                const char next = buf[i + 1];
                if ((ch == '!' && (next == '=' || next == '~')) || ((ch == '>' || ch == '<') && next == '=')) {
                    op.push_back(next);
                    ++i;
                }
            }
            tokens.push_back(JqlToken{std::move(op), 0, false});
            ++i;
            continue;
        }
        int j = i;
        while (j < end && IsJqlIdChar(static_cast<unsigned char>(buf[j]))) {
            ++j;
        }
        if (j == i) {
            tokens.push_back(JqlToken{std::string(1, buf[i]), 0, false});
            ++i;
            continue;
        }
        tokens.push_back(JqlToken{std::string(buf + i, buf + j), 0, false});
        i = j;
    }
    return tokens;
}

static bool TokenEquals(const JqlToken& token, const char* text) {
    return token.Punct == 0 && !token.Quoted && AsciiEqualsIgnoreCaseToLowered(token.Text, ToLowerAsciiCopy(text));
}

static bool TokenIsPunct(const JqlToken& token, char ch) { return token.Punct == ch; }

static bool TokenIsJqlOperator(const JqlToken& token) {
    if (token.Punct != 0 || token.Quoted) {
        return false;
    }
    const std::string t = ToLowerAsciiCopy(token.Text);
    return t == "=" || t == "!=" || t == "~" || t == "!~" || t == ">" || t == ">=" || t == "<" || t == "<=" ||
           t == "in" || t == "is" || t == "was" || t == "changed";
}

static const TrackerField* FindValueFieldForJqlContext(const std::vector<JqlToken>& tokens, const AppController& app) {
    for (int i = static_cast<int>(tokens.size()) - 1; i >= 0; --i) {
        if (!TokenIsJqlOperator(tokens[static_cast<size_t>(i)])) {
            continue;
        }
        int fieldIndex = i - 1;
        while (fieldIndex >= 0 && (TokenEquals(tokens[static_cast<size_t>(fieldIndex)], "NOT") ||
                                   TokenEquals(tokens[static_cast<size_t>(fieldIndex)], "WAS"))) {
            --fieldIndex;
        }
        while (fieldIndex >= 0 && tokens[static_cast<size_t>(fieldIndex)].Punct != 0) {
            --fieldIndex;
        }
        if (fieldIndex >= 0) {
            return FindTrackerFieldForJqlToken(app, tokens[static_cast<size_t>(fieldIndex)].Text);
        }
    }
    return nullptr;
}

enum class JqlSuggestMode {
    Field,
    Operator,
    Value,
    IsOperand,
    Logical,
    OrderField,
    OrderByKeyword,
    SortDirection,
    None
};

static int FindLastOrderByToken(const std::vector<JqlToken>& tokens) {
    for (int i = static_cast<int>(tokens.size()) - 2; i >= 0; --i) {
        if (TokenEquals(tokens[static_cast<size_t>(i)], "ORDER") &&
            TokenEquals(tokens[static_cast<size_t>(i + 1)], "BY")) {
            return i;
        }
    }
    return -1;
}

static JqlSuggestMode DetermineJqlSuggestMode(const std::vector<JqlToken>& tokens, const std::string& prefix,
                                              const AppController& app, const TrackerField** outValueField) {
    if (outValueField) {
        *outValueField = FindValueFieldForJqlContext(tokens, app);
    }
    if (tokens.empty()) {
        return JqlSuggestMode::Field;
    }

    const int orderBy = FindLastOrderByToken(tokens);
    if (orderBy >= 0 && orderBy + 1 < static_cast<int>(tokens.size())) {
        const JqlToken& last = tokens.back();
        if (static_cast<int>(tokens.size()) == orderBy + 2 || TokenIsPunct(last, ',')) {
            return JqlSuggestMode::OrderField;
        }
        if (TokenEquals(last, "ASC") || TokenEquals(last, "DESC")) {
            return prefix.empty() ? JqlSuggestMode::None : JqlSuggestMode::OrderField;
        }
        return JqlSuggestMode::SortDirection;
    }

    const JqlToken& last = tokens.back();
    if (TokenIsPunct(last, '(') || TokenIsPunct(last, ',')) {
        return (outValueField != nullptr && *outValueField != nullptr) ? JqlSuggestMode::Value : JqlSuggestMode::Field;
    }
    if (TokenEquals(last, "AND") || TokenEquals(last, "OR") || TokenEquals(last, "NOT")) {
        if (TokenEquals(last, "NOT") && tokens.size() >= 2 && TokenEquals(tokens[tokens.size() - 2], "IS")) {
            return JqlSuggestMode::IsOperand;
        }
        return JqlSuggestMode::Field;
    }
    if (TokenEquals(last, "ORDER")) {
        return JqlSuggestMode::OrderByKeyword;
    }
    if (TokenEquals(last, "IS")) {
        return JqlSuggestMode::IsOperand;
    }
    if (TokenEquals(last, "IN") || TokenIsJqlOperator(last)) {
        return TokenEquals(last, "IS") ? JqlSuggestMode::IsOperand : JqlSuggestMode::Value;
    }
    if (FindTrackerFieldForJqlToken(app, last.Text) != nullptr && !last.Quoted) {
        return JqlSuggestMode::Operator;
    }
    return JqlSuggestMode::Logical;
}

static void BuildJqlSuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd, AppController& app,
                                JqlSuggestBuild& out) {
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
    const TrackerField* valueField = nullptr;

    if (selStart != selEnd) {
        const int lo = (std::min)(selStart, selEnd);
        const int hi = (std::max)(selStart, selEnd);
        replaceStart = lo;
        replaceEnd = hi;
        prefix.assign(buf + lo, buf + hi);
    } else {
        bool inString = false;
        int stringOpen = -1;
        JqlScanStringStateToCursor(buf, bufLen, cursor, inString, stringOpen);
        if (inString && stringOpen >= 0 && cursor > stringOpen + 1) {
            replaceStart = stringOpen + 1;
            replaceEnd = cursor;
            prefix.assign(buf + replaceStart, buf + replaceEnd);
        } else {
            int L = cursor;
            int R = cursor;
            while (L > 0 && IsJqlIdChar(static_cast<unsigned char>(buf[L - 1]))) {
                --L;
            }
            while (R < bufLen && IsJqlIdChar(static_cast<unsigned char>(buf[R]))) {
                ++R;
            }
            replaceStart = L;
            replaceEnd = R;
            prefix.assign(buf + L, buf + R);
        }
    }

    out.ReplaceStart = replaceStart;
    out.ReplaceEnd = replaceEnd;

    const std::vector<JqlToken> leftTokens = TokenizeJqlPrefix(buf, replaceStart);
    const JqlSuggestMode mode = DetermineJqlSuggestMode(leftTokens, prefix, app, &valueField);
    static const char* kClausePrefixes[] = {"NOT"};
    static const char* kOperators[] = {"=", "!=", "IN", "NOT IN", "IS",  "IS NOT", "~",      "!~",
                                       ">", ">=", "<",  "<=",     "WAS", "WAS IN", "CHANGED"};
    static const char* kValueFunctions[] = {"currentUser()", "membersOf()"};
    static const char* kIsOperands[] = {"EMPTY", "NULL"};
    static const char* kLogical[] = {"AND", "OR", "ORDER BY"};
    static const char* kSortDirections[] = {"ASC", "DESC"};
    static const char* kOrderByTail[] = {"BY"};

    if (mode == JqlSuggestMode::Field || mode == JqlSuggestMode::OrderField) {
        if (mode == JqlSuggestMode::Field) {
            AppendJqlTerms(prefix, kClausePrefixes,
                           static_cast<int>(sizeof(kClausePrefixes) / sizeof(kClausePrefixes[0])), out.Items, seen);
        }
        AppendFieldCatalogSuggestions(app, prefix, out.Items, seen);
    } else if (mode == JqlSuggestMode::Operator) {
        AppendJqlTerms(prefix, kOperators, static_cast<int>(sizeof(kOperators) / sizeof(kOperators[0])), out.Items,
                       seen);
    } else if (mode == JqlSuggestMode::Value) {
        if (valueField != nullptr && (!valueField->AllowedValueOptions.empty() || !valueField->AllowedValues.empty())) {
            AppendValueSuggestions(*valueField, prefix, out.Items, seen);
        }
        if (valueField != nullptr && IsJqlUserField(*valueField)) {
            AppendJqlTerms(prefix, kValueFunctions,
                           static_cast<int>(sizeof(kValueFunctions) / sizeof(kValueFunctions[0])), out.Items, seen);
        }
    } else if (mode == JqlSuggestMode::IsOperand) {
        AppendJqlTerms(prefix, kIsOperands, static_cast<int>(sizeof(kIsOperands) / sizeof(kIsOperands[0])), out.Items,
                       seen);
    } else if (mode == JqlSuggestMode::Logical) {
        AppendJqlTerms(prefix, kLogical, static_cast<int>(sizeof(kLogical) / sizeof(kLogical[0])), out.Items, seen);
    } else if (mode == JqlSuggestMode::OrderByKeyword) {
        AppendJqlTerms(prefix, kOrderByTail, static_cast<int>(sizeof(kOrderByTail) / sizeof(kOrderByTail[0])),
                       out.Items, seen);
    } else if (mode == JqlSuggestMode::SortDirection) {
        AppendJqlTerms(prefix, kSortDirections, static_cast<int>(sizeof(kSortDirections) / sizeof(kSortDirections[0])),
                       out.Items, seen);
    }

    auto labelLessAscii = [](const JqlSuggestion& a, const JqlSuggestion& b) {
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

struct JqlInputCallbackUserData {
    UiDrawSession* session = nullptr;
    AppController* app = nullptr;
    JqlSuggestBuild* suggestBuild = nullptr;
};

static bool QueueJqlApplyFromBuild(UiDrawSession& d, const JqlSuggestBuild& b, int index, bool fromMousePick) {
    if (index < 0 || index >= static_cast<int>(b.Items.size())) {
        return false;
    }
    d.jqlAcpReplaceStart = b.ReplaceStart;
    d.jqlAcpReplaceEnd = b.ReplaceEnd;
    d.jqlAcpReplaceText = b.Items[static_cast<size_t>(index)].Insert;
    d.jqlAcpApplyReplace = true;
    d.jqlAcpWantsJqlInputFocus = true;
    d.jqlAcpPendingMouseCaretAfterPick = fromMousePick;
    return true;
}

static void JqlAcpTryFlushPendingToViewJql(UiDrawSession& d) {
    if (!d.jqlAcpApplyReplace) {
        return;
    }
    if (d.jqlAcpReplaceStart < 0 || d.jqlAcpReplaceEnd < d.jqlAcpReplaceStart) {
        d.jqlAcpApplyReplace = false;
        d.jqlAcpReplaceStart = -1;
        d.jqlAcpReplaceEnd = -1;
        d.jqlAcpReplaceText.clear();
        d.jqlAcpPendingMouseCaretAfterPick = false;
        return;
    }
    std::string s(d.viewJqlBuf);
    if (static_cast<size_t>(d.jqlAcpReplaceStart) > s.size() || static_cast<size_t>(d.jqlAcpReplaceEnd) > s.size()) {
        d.jqlAcpApplyReplace = false;
        d.jqlAcpReplaceStart = -1;
        d.jqlAcpReplaceEnd = -1;
        d.jqlAcpReplaceText.clear();
        d.jqlAcpPendingMouseCaretAfterPick = false;
        return;
    }
    const int rs = d.jqlAcpReplaceStart;
    const std::string ins = d.jqlAcpReplaceText;
    const bool mouseCaret = d.jqlAcpPendingMouseCaretAfterPick;
    s.replace(static_cast<size_t>(d.jqlAcpReplaceStart), static_cast<size_t>(d.jqlAcpReplaceEnd - d.jqlAcpReplaceStart),
              ins);
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlBuf, s);
    d.jqlAcpListSelected = 0;
    d.jqlAcpApplyReplace = false;
    d.jqlAcpReplaceStart = -1;
    d.jqlAcpReplaceEnd = -1;
    d.jqlAcpReplaceText.clear();
    d.jqlAcpPendingMouseCaretAfterPick = false;
    if (mouseCaret) {
        d.jqlAcpWantsCursorPos = rs + static_cast<int>(ins.size());
    }
}

static int JqlInputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* ud = static_cast<JqlInputCallbackUserData*>(data->UserData);
    UiDrawSession* d = ud != nullptr ? ud->session : nullptr;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && d != nullptr && d->jqlAcpWantsCursorPos >= 0) {
        int p = d->jqlAcpWantsCursorPos;
        if (p < 0) {
            p = 0;
        }
        if (p > data->BufTextLen) {
            p = data->BufTextLen;
        }
        data->CursorPos = p;
        data->SelectionStart = data->SelectionEnd = p;
        d->jqlAcpLastCursor = p;
        d->jqlAcpLastSelectionStart = p;
        d->jqlAcpLastSelectionEnd = p;
        d->jqlAcpWantsCursorPos = -1;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (ud != nullptr && ud->app != nullptr && ud->suggestBuild != nullptr) {
            BuildJqlSuggestions(data->Buf, data->BufTextLen, data->CursorPos, data->SelectionStart, data->SelectionEnd,
                                *ud->app, *ud->suggestBuild);
            if (d != nullptr) {
                d->jqlAcpLastCursor = data->CursorPos;
                d->jqlAcpLastSelectionStart = data->SelectionStart;
                d->jqlAcpLastSelectionEnd = data->SelectionEnd;
            }
            const int n = static_cast<int>(ud->suggestBuild->Items.size());
            if (d != nullptr && n > 0) {
                if (data->EventKey == ImGuiKey_DownArrow) {
                    d->jqlAcpListSelected = (std::min)(n - 1, d->jqlAcpListSelected + 1);
                    d->jqlAcpScrollToSelected = true;
                } else if (data->EventKey == ImGuiKey_UpArrow) {
                    d->jqlAcpListSelected = (std::max)(0, d->jqlAcpListSelected - 1);
                    d->jqlAcpScrollToSelected = true;
                } else {
                    d->jqlAcpListSelected = (std::max)(0, (std::min)(d->jqlAcpListSelected, n - 1));
                }
            } else if (d != nullptr) {
                d->jqlAcpListSelected = 0;
            }
        }
        return 0;
    }

    if (d != nullptr) {
        d->jqlAcpLastCursor = data->CursorPos;
        d->jqlAcpLastSelectionStart = data->SelectionStart;
        d->jqlAcpLastSelectionEnd = data->SelectionEnd;
    }

    if (ud != nullptr && ud->app != nullptr && ud->suggestBuild != nullptr &&
        data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        BuildJqlSuggestions(data->Buf, data->BufTextLen, data->CursorPos, data->SelectionStart, data->SelectionEnd,
                            *ud->app, *ud->suggestBuild);
        const int n = static_cast<int>(ud->suggestBuild->Items.size());
        if (d != nullptr) {
            const bool enterDown =
                ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            if (n > 0) {
                d->jqlAcpListSelected = (std::max)(0, (std::min)(d->jqlAcpListSelected, n - 1));
                if (enterDown) {
                    QueueJqlApplyFromBuild(*d, *ud->suggestBuild, d->jqlAcpListSelected, false);
                }
            } else {
                d->jqlAcpListSelected = 0;
                if (enterDown) {
                    d->jqlWantsApplyFromEnter = true;
                }
            }
        }
    }

    return 0;
}

} // namespace

namespace SmatchetViewsDashboardUiDetail {
void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory) {
    ConfigManager::Save(d.cfg);
    if (pushHistory)
        d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
    app.SyncWithBackend(&d.cfg, &store);
}

void ApplyViewsActiveJqlFromBuffers(AppController& app, UiDrawSession& d, Views& viewState,
                                           const ViewDefinition& activeView) {
    ViewDefinition updated = activeView;
    updated.Name = d.viewNameBuf;
    updated.Jql = d.viewJqlBuf;
    updated.Fields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
    updated.ColumnOrder = d.editingColumnOrder;
    if (viewState.UpdateActive(updated)) {
        d.cfg.JqlQuery = updated.Jql;
        d.cfg.SelectedFields = updated.Fields;
        SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, viewState.GetStore(), true);
    }
}

void DrawJqlQueryEditor(AppController& app, UiDrawSession& d, Views& viewState, const ViewDefinition& activeView) {
        const std::string currentJql(d.viewJqlBuf);
        const bool disableOpenJql = currentJql.empty() || d.cfg.Domain.empty();
        if (disableOpenJql) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("JQL")) {
            app.OpenUrl(app.BuildJqlSearchUrl(d.cfg, currentJql));
        }
        if (disableOpenJql) {
            ImGui::EndDisabled();
        }
        ImGui::SetItemTooltip("Open this JQL in Jira.");
        ImGui::SameLine();
        JqlSuggestBuild jqlSuggestBuild;
        JqlInputCallbackUserData jqlCb{};
        jqlCb.session = &d;
        jqlCb.app = &app;
        jqlCb.suggestBuild = &jqlSuggestBuild;
        if (d.jqlAcpWantsJqlInputFocus) {
            ImGui::SetKeyboardFocusHere(0);
            d.jqlAcpWantsJqlInputFocus = false;
        }
        bool jqlInputHot = false;
        ImVec2 jqlFieldRectMin{};
        ImVec2 jqlFieldRectSize{};
        {
            const float applyBtnW = ImGui::CalcTextSize("Apply JQL").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float inputW = ImGui::GetContentRegionAvail().x - applyBtnW - spacing;
            ImGui::SetNextItemWidth((std::max)(80.0f, inputW));
            ImGui::InputText("##JQL", d.viewJqlBuf, sizeof(d.viewJqlBuf),
                             ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackHistory,
                             &JqlInputTextCallback, &jqlCb);
            jqlInputHot = ImGui::IsItemActive() || ImGui::IsItemFocused();
            jqlFieldRectMin = ImGui::GetItemRectMin();
            jqlFieldRectSize = ImGui::GetItemRectSize();
            const bool suppressJqlHintTooltip = jqlInputHot || (d.viewJqlBuf[0] != '\0');
            if (!suppressJqlHintTooltip) {
                ImGui::SetItemTooltip(
                    "Atlassian JQL used when fetching issues.\n"
                    "Up/Down: suggestion list. Enter: insert suggestion (when listed), else run query.\n"
                    "Click row: apply suggestion.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply JQL")) {
                SmatchetViewsDashboardUiDetail::ApplyViewsActiveJqlFromBuffers(app, d, viewState, activeView);
            }
            ImGui::SetItemTooltip("Save this JQL on the active view and sync issues.");
        }
        if (d.jqlWantsApplyFromEnter) {
            d.jqlWantsApplyFromEnter = false;
            SmatchetViewsDashboardUiDetail::ApplyViewsActiveJqlFromBuffers(app, d, viewState, activeView);
        }

        const bool jqlAcpOpen = jqlInputHot && !jqlSuggestBuild.Items.empty();
        if (jqlAcpOpen) {
            ImGui::SetNextWindowPos(ImVec2(jqlFieldRectMin.x, jqlFieldRectMin.y + jqlFieldRectSize.y),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(jqlFieldRectSize.x, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
            ImGui::Begin("##JqlAcpPopup", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::BeginChild("##JqlAcpScroll", ImVec2(0, 168), true);
            const int n = static_cast<int>(jqlSuggestBuild.Items.size());
            const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
            const bool mouseMoved = mouseDelta.x != 0.0f || mouseDelta.y != 0.0f;
            for (int i = 0; i < n; ++i) {
                const bool sel = (i == d.jqlAcpListSelected);
                const ImGuiSelectableFlags flags = ImGuiSelectableFlags_NoAutoClosePopups |
                                                   (sel ? ImGuiSelectableFlags_Highlight : ImGuiSelectableFlags_None);
                const bool rowPress =
                    ImGui::Selectable(jqlSuggestBuild.Items[static_cast<size_t>(i)].Label.c_str(), sel, flags);
                const bool reclickSelected = sel && ImGui::IsItemClicked(0);
                if (rowPress || reclickSelected) {
                    QueueJqlApplyFromBuild(d, jqlSuggestBuild, i, true);
                }
                if (sel) {
                    ImGui::SetItemDefaultFocus();
                    if (d.jqlAcpScrollToSelected) {
                        ImGui::SetScrollHereY(0.5f);
                        d.jqlAcpScrollToSelected = false;
                    }
                }
                if (mouseMoved && ImGui::IsItemHovered()) {
                    d.jqlAcpListSelected = i;
                }
            }
            ImGui::EndChild();
            ImGui::End();
            ImGui::PopStyleVar();
        }
        JqlAcpTryFlushPendingToViewJql(d);

}
} // namespace SmatchetViewsDashboardUiDetail
