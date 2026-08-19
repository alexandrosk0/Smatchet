#include "JqlSuggestEnginePure.h"

#include "StringUtil.h"
#include "Tracker/JqlEscape.h"
#include "Tracker/TrackerQuerySuggestCommon.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

// SMATCHET_DEVIATION(rule=duplication; reason=shared-helper using-block; owner=tracker-backend; revisit=2026-12-31)
using tracker_query_suggest::AddSuggestionUnique;
using tracker_query_suggest::AppendFieldCatalog;
using tracker_query_suggest::AppendTerms;
using tracker_query_suggest::AppendValueSuggestions;
using tracker_query_suggest::AsciiEqualsIgnoreCaseToLowered;
using tracker_query_suggest::AsciiStartsWithIgnoreCase;
using tracker_query_suggest::BeginQuerySuggestPass;
using tracker_query_suggest::FindTrackerField;
using tracker_query_suggest::IsQueryDateField;
using tracker_query_suggest::IsQueryIdChar;
using tracker_query_suggest::IsQueryUserField;
using tracker_query_suggest::SortAndCapQuerySuggestions;

// Jira's wording for the display-name variant of a user-field value suggestion. The sole
// backend-local divergence in the otherwise shared AppendValueSuggestions body — Plane says
// " (display) -> ". Kept verbatim; only the body is now single-sourced.
constexpr const char* kJiraUserDisplaySuffix = " (display name) -> ";

static bool IsJqlVersionField(const TrackerField& field) {
    return field.Type == "version" || field.ItemsType == "version";
}

static bool IsJqlSprintField(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::Sprint || field.Type == "sprint" || field.ItemsType == "sprint";
}

namespace JqlFnMask {
constexpr unsigned User = 1u << 0;
constexpr unsigned Date = 1u << 1;
constexpr unsigned Version = 1u << 2;
constexpr unsigned Sprint = 1u << 3;
} // namespace JqlFnMask

struct JqlFunctionSpec {
    const char* Label;   // popup display (e.g. `membersOf("…")`)
    const char* Insert;  // text actually inserted; "\x7F" marks the post-insert caret
    unsigned FamilyMask; // which field families this function applies to
};

// Static catalog. Keep alphabetical within each family for popup readability.
static const JqlFunctionSpec kJqlFunctions[] = {
    // User-field functions.
    {"currentUser()", "currentUser()", JqlFnMask::User},
    {"membersOf(\"…\")", "membersOf(\"\x7F\")", JqlFnMask::User},
    // Date-field functions.
    {"endOfDay()", "endOfDay()", JqlFnMask::Date},
    {"endOfMonth()", "endOfMonth()", JqlFnMask::Date},
    {"endOfWeek()", "endOfWeek()", JqlFnMask::Date},
    {"endOfYear()", "endOfYear()", JqlFnMask::Date},
    {"now()", "now()", JqlFnMask::Date},
    {"startOfDay()", "startOfDay()", JqlFnMask::Date},
    {"startOfMonth()", "startOfMonth()", JqlFnMask::Date},
    {"startOfWeek()", "startOfWeek()", JqlFnMask::Date},
    {"startOfYear()", "startOfYear()", JqlFnMask::Date},
    // Version-field functions.
    {"earliestUnreleasedVersion()", "earliestUnreleasedVersion()", JqlFnMask::Version},
    {"latestReleasedVersion()", "latestReleasedVersion()", JqlFnMask::Version},
    {"releasedVersions()", "releasedVersions()", JqlFnMask::Version},
    {"unreleasedVersions()", "unreleasedVersions()", JqlFnMask::Version},
    // Sprint-field functions.
    {"closedSprints()", "closedSprints()", JqlFnMask::Sprint},
    {"futureSprints()", "futureSprints()", JqlFnMask::Sprint},
    {"openSprints()", "openSprints()", JqlFnMask::Sprint},
};

static unsigned JqlFieldFamilyMask(const TrackerField& field) {
    unsigned mask = 0;
    if (IsQueryUserField(field)) {
        mask |= JqlFnMask::User;
    }
    if (IsQueryDateField(field)) {
        mask |= JqlFnMask::Date;
    }
    if (IsJqlVersionField(field)) {
        mask |= JqlFnMask::Version;
    }
    if (IsJqlSprintField(field)) {
        mask |= JqlFnMask::Sprint;
    }
    return mask;
}

static void AppendJqlFunctionSuggestions(const TrackerField& field, const std::string& prefix,
                                         std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen) {
    const unsigned fieldMask = JqlFieldFamilyMask(field);
    if (fieldMask == 0) {
        return;
    }
    const std::string pre = ToLowerAsciiCopy(prefix);
    for (const JqlFunctionSpec& spec : kJqlFunctions) {
        if ((spec.FamilyMask & fieldMask) == 0) {
            continue;
        }
        // Prefix-match against the label (which mirrors the function name without the
        // caret sentinel) so users can type "memb" and see `membersOf("…")`.
        if (!AsciiStartsWithIgnoreCase(spec.Label, pre)) {
            continue;
        }
        AddSuggestionUnique(out, seen, std::string(spec.Label), std::string(spec.Insert));
    }
}

/// True when `user` is a real human account (not a Jira Cloud Connect/Forge app or a JSM
/// portal customer). AccountType is empty when the backend didn't surface it — treat that
/// as "include" (older Jira API responses + non-Jira backends).
static bool IsNonSystemTrackerUser(const TrackerUser& user) {
    if (!user.Active) {
        return false;
    }
    if (user.AccountType.empty()) {
        return true;
    }
    return user.AccountType != "app" && user.AccountType != "customer";
}

/// Build the JQL value-token for a user. Prefer the display name (more readable in the
/// query), else the accountId. Both originate from the tracker server, so both are
/// JQL-escaped through tracker_jql::QuoteLiteral before the surrounding quotes are added —
/// a `"` or `\` in either field is escaped, never allowed to break out of the literal
/// (security: H3 + E1).
static std::string BuildJqlUserInsert(const TrackerUser& user) {
    if (!user.DisplayName.empty()) {
        return "\"" + tracker_jql::QuoteLiteral(user.DisplayName) + "\"";
    }
    if (!user.AccountId.empty()) {
        return "\"" + tracker_jql::QuoteLiteral(user.AccountId) + "\"";
    }
    return user.DisplayName;
}

/// Emit non-system users from the cached catalog as JQL value suggestions, matched
/// case-insensitively anywhere inside display name + email — typing a surname ("smith")
/// or a mail domain finds the account, not just typing the leading characters. Prefix
/// matches take the slots first so the cap can't crowd them out behind mid-name hits.
/// Capped at a generous limit so the popup stays responsive on tenants with thousands
/// of users.
static void AppendJqlUserCatalogSuggestions(const std::vector<TrackerUser>& users, const std::string& prefix,
                                            std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen) {
    // Empty-prefix bail-out: on tenants with hundreds of users, dumping an alphabetic slice
    // of the org chart on `assignee = ` adds noise without value. The legacy `currentUser()`
    // / `membersOf()` function suggestions still appear in this case (they're appended by
    // AppendJqlFunctionSuggestions). Real name matching kicks in as soon as the user types
    // the first character.
    if (prefix.empty()) {
        return;
    }
    const std::string pre = ToLowerAsciiCopy(prefix);
    constexpr int kMaxUsers = 50;
    int added = 0;
    // Mid-name matches are held back rather than walked in a second pass: this runs on every
    // omnibox keystroke over the whole catalog (Pillar 1), so each user is case-folded at
    // most once per field and the email fold is skipped whenever the name already leads.
    std::vector<QuerySuggestion> midMatches;
    for (const auto& user : users) {
        if (!IsNonSystemTrackerUser(user)) {
            continue;
        }
        const std::string nameLower = ToLowerAsciiCopy(user.DisplayName);
        const size_t namePos = nameLower.find(pre);
        size_t emailPos = std::string::npos;
        if (namePos != 0 && !user.EmailAddress.empty()) {
            emailPos = ToLowerAsciiCopy(user.EmailAddress).find(pre);
        }
        if (namePos == std::string::npos && emailPos == std::string::npos) {
            continue;
        }
        std::string label = user.DisplayName;
        if (!user.EmailAddress.empty()) {
            label += " (" + user.EmailAddress + ")";
        }
        const bool startsWith = namePos == 0 || emailPos == 0;
        if (!startsWith) {
            // Held-back matches beyond the cap can never be shown (prefix matches only ever
            // take slots away from them), so a hot substring like "a" can't grow this vector
            // past kMaxUsers entries.
            if (static_cast<int>(midMatches.size()) < kMaxUsers) {
                midMatches.push_back(QuerySuggestion{std::move(label), BuildJqlUserInsert(user)});
            }
            continue;
        }
        if (added >= kMaxUsers) {
            continue;
        }
        const size_t before = out.size();
        AddSuggestionUnique(out, seen, std::move(label), BuildJqlUserInsert(user));
        // AddSuggestionUnique drops empty / duplicate inserts, so only entries that actually
        // reached the list consume a slot.
        if (out.size() != before) {
            ++added;
        }
    }
    for (auto& mid : midMatches) {
        if (added >= kMaxUsers) {
            break;
        }
        const size_t before = out.size();
        AddSuggestionUnique(out, seen, std::move(mid.Label), std::move(mid.Insert));
        if (out.size() != before) {
            ++added;
        }
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
        while (j < end && IsQueryIdChar(static_cast<unsigned char>(buf[j]))) {
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

static const TrackerField* FindValueFieldForJqlContext(const std::vector<JqlToken>& tokens,
                                                       const std::vector<TrackerField>& fields) {
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
            return FindTrackerField(fields, tokens[static_cast<size_t>(fieldIndex)].Text);
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
                                              const std::vector<TrackerField>& fields,
                                              const TrackerField** outValueField) {
    if (outValueField) {
        *outValueField = FindValueFieldForJqlContext(tokens, fields);
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
    if (FindTrackerField(fields, last.Text) != nullptr && !last.Quoted) {
        return JqlSuggestMode::Operator;
    }
    return JqlSuggestMode::Logical;
}

void AppendJqlValueModeSuggestions(const std::vector<TrackerUser>& users, const std::string& prefix,
                                   const TrackerField* valueField, QuerySuggestBuild& out,
                                   std::unordered_set<std::string>& seen, QuerySuggestMeta* metaOut) {
    if (valueField != nullptr && (!valueField->AllowedValueOptions.empty() || !valueField->AllowedValues.empty())) {
        AppendValueSuggestions(*valueField, prefix, kJiraUserDisplaySuffix, out.Items, seen);
    }
    if (valueField != nullptr) {
        AppendJqlFunctionSuggestions(*valueField, prefix, out.Items, seen);
    }
    // For user fields, also surface the cached non-system users from the catalog fetch.
    // The async live-user search (driven via metaOut->UserValueToken below) still runs
    // and merges results when the prefix is long enough — this just makes the offline
    // catalog list available immediately without any network round-trip.
    if (valueField != nullptr && IsQueryUserField(*valueField)) {
        AppendJqlUserCatalogSuggestions(users, prefix, out.Items, seen);
    }
    if (metaOut != nullptr && valueField != nullptr && IsQueryUserField(*valueField)) {
        metaOut->UserValueToken = true;
        metaOut->UserSearchPrefix = prefix;
    }
}

// Dispatch the resolved suggest mode onto the matching term/value appenders.
void AppendJqlSuggestionsForMode(JqlSuggestMode mode, const std::vector<TrackerField>& fields,
                                 const std::vector<TrackerUser>& users, const std::string& prefix,
                                 const TrackerField* valueField, QuerySuggestBuild& out,
                                 std::unordered_set<std::string>& seen, QuerySuggestMeta* metaOut) {
    if (mode == JqlSuggestMode::Field || mode == JqlSuggestMode::OrderField) {
        if (mode == JqlSuggestMode::Field) {
            static const char* kClausePrefixes[] = {"NOT"};
            AppendTerms(prefix, kClausePrefixes, static_cast<int>(sizeof(kClausePrefixes) / sizeof(kClausePrefixes[0])),
                        out.Items, seen);
        }
        AppendFieldCatalog(fields, prefix, out.Items, seen);
    } else if (mode == JqlSuggestMode::Operator) {
        static const char* kOperators[] = {"=", "!=", "IN", "NOT IN", "IS",  "IS NOT", "~",      "!~",
                                           ">", ">=", "<",  "<=",     "WAS", "WAS IN", "CHANGED"};
        AppendTerms(prefix, kOperators, static_cast<int>(sizeof(kOperators) / sizeof(kOperators[0])), out.Items, seen);
    } else if (mode == JqlSuggestMode::Value) {
        AppendJqlValueModeSuggestions(users, prefix, valueField, out, seen, metaOut);
    } else if (mode == JqlSuggestMode::IsOperand) {
        static const char* kIsOperands[] = {"EMPTY", "NULL"};
        AppendTerms(prefix, kIsOperands, static_cast<int>(sizeof(kIsOperands) / sizeof(kIsOperands[0])), out.Items,
                    seen);
    } else if (mode == JqlSuggestMode::Logical) {
        if (!prefix.empty()) {
            static const char* kLogical[] = {"AND", "OR", "ORDER BY"};
            AppendTerms(prefix, kLogical, static_cast<int>(sizeof(kLogical) / sizeof(kLogical[0])), out.Items, seen);
        }
    } else if (mode == JqlSuggestMode::OrderByKeyword) {
        static const char* kOrderByTail[] = {"BY"};
        AppendTerms(prefix, kOrderByTail, static_cast<int>(sizeof(kOrderByTail) / sizeof(kOrderByTail[0])), out.Items,
                    seen);
    } else if (mode == JqlSuggestMode::SortDirection) {
        static const char* kSortDirections[] = {"ASC", "DESC"};
        AppendTerms(prefix, kSortDirections, static_cast<int>(sizeof(kSortDirections) / sizeof(kSortDirections[0])),
                    out.Items, seen);
    }
}

} // namespace

void BuildJqlSuggestionsPure(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                             const std::vector<TrackerField>& fields, const std::vector<TrackerUser>& users,
                             QuerySuggestBuild& out, QuerySuggestMeta* metaOut) {
    // SMATCHET_DEVIATION(rule=duplication; reason=engine entry scaffolding; owner=tracker-backend; revisit=2026-12-31)
    std::unordered_set<std::string> seen;
    int replaceStart = 0;
    int replaceEnd = 0;
    std::string prefix;
    if (!BeginQuerySuggestPass(buf, bufLen, cursor, selStart, selEnd, out, metaOut, replaceStart, replaceEnd, prefix)) {
        return;
    }
    const TrackerField* valueField = nullptr;

    const std::vector<JqlToken> leftTokens = TokenizeJqlPrefix(buf, replaceStart);
    const JqlSuggestMode mode = DetermineJqlSuggestMode(leftTokens, prefix, fields, &valueField);

    AppendJqlSuggestionsForMode(mode, fields, users, prefix, valueField, out, seen, metaOut);

    SortAndCapQuerySuggestions(out.Items);
}