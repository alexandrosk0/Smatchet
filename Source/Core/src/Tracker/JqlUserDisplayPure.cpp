#include "Tracker/JqlUserDisplayPure.h"

#include "Tracker/TrackerFieldSchema.h"
#include "Tracker/TrackerQuerySuggestCommon.h"

#include <cctype>

namespace jql_user_display {

namespace {

bool IsHexDigit(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isxdigit(uc) != 0;
}

bool IsAllHex(const std::string& s, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        if (!IsHexDigit(s[i])) {
            return false;
        }
    }
    return true;
}

/// 8-4-4-4-12 hex with hyphens — the body of every Jira Cloud accountId.
bool IsUuid(const std::string& s) {
    if (s.size() != 36) {
        return false;
    }
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') {
        return false;
    }
    return IsAllHex(s, 0, 8) && IsAllHex(s, 9, 13) && IsAllHex(s, 14, 18) && IsAllHex(s, 19, 23) && IsAllHex(s, 24, 36);
}

/// The 24-char legacy object id older / non-Cloud deployments emit as an account key.
/// Alphanumeric rather than strict hex (Atlassian's documented example ends in `…ede21g`),
/// with at least one digit so an ordinary 24-letter word never reads as an id.
bool IsLegacyId24(const std::string& s) {
    if (s.size() != 24) {
        return false;
    }
    bool anyDigit = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char uc = static_cast<unsigned char>(s[i]);
        if (std::isdigit(uc) != 0) {
            anyDigit = true;
        } else if (std::isalpha(uc) == 0) {
            return false;
        }
    }
    return anyDigit;
}

/// Characters that make up a BARE (unquoted) account token. ':' is in the set because a
/// Cloud accountId carries the instance discriminator (`712020:<uuid>`) — the query
/// grammar's own identifier run stops at the colon and would split the id in half.
bool IsBareTokenChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '.' || c == '-' || c == ':';
}

/// Resolve `accountId` against the catalog. Returns the display name, or "" when the
/// account is unknown or has no name to show.
std::string LookupDisplayName(const std::string& accountId, const std::vector<TrackerUser>& users) {
    for (size_t i = 0; i < users.size(); ++i) {
        if (users[i].AccountId == accountId && !users[i].DisplayName.empty()) {
            return users[i].DisplayName;
        }
    }
    return std::string();
}

/// Consume the double-quoted string opening at `start`. Returns the index one past the
/// closing quote (or the end of input for an unterminated string) and fills `outValue`
/// with the unescaped contents.
size_t ScanQuotedValue(const std::string& s, size_t start, std::string& outValue) {
    outValue.clear();
    size_t i = start + 1;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            outValue.push_back(s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '"') {
            return i + 1;
        }
        outValue.push_back(c);
        ++i;
    }
    return s.size();
}

/// Walk `query`'s value tokens (double-quoted strings and bare identifier runs) and call
/// `onToken(token, spanStart, spanEnd)` for each; bytes outside any token are reported
/// through `onOther(byte)`. Single tokenizer shared by the render pass and the
/// unresolved-id collector so their notion of a token cannot drift.
template <typename TokenFn, typename OtherFn>
void ForEachValueToken(const std::string& query, TokenFn onToken, OtherFn onOther) {
    std::string token;
    size_t i = 0;
    while (i < query.size()) {
        size_t next = i + 1;
        if (query[i] == '"') {
            next = ScanQuotedValue(query, i, token);
        } else if (IsBareTokenChar(query[i])) {
            next = i;
            while (next < query.size() && IsBareTokenChar(query[next])) {
                ++next;
            }
            token.assign(query, i, next - i);
        } else {
            onOther(query[i]);
            i = next;
            continue;
        }
        onToken(token, i, next);
        i = next;
    }
}

} // namespace

bool LooksLikeAccountId(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    if (IsUuid(token) || IsLegacyId24(token)) {
        return true;
    }
    const size_t colon = token.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    // "<discriminator>:<uuid>" (Cloud) and "qm:<uuid>:<uuid>" (Service Management portal
    // accounts) both end in a UUID — the last colon separates it from whatever prefixes it.
    return IsUuid(token.substr(colon + 1));
}

namespace {

/// Lower-case ASCII copy for the keyword / name comparisons below.
std::string AsciiLowered(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

/// True when `tokenLowered` is a clause boundary that ends any field's value position
/// (`AND`, `OR`, `ORDER BY`).
bool IsClauseBreakKeyword(const std::string& tokenLowered) {
    return tokenLowered == "and" || tokenLowered == "or" || tokenLowered == "order";
}

/// Operator / literal keywords that may legally sit between a user field and its value(s)
/// (`assignee IN (…)`, `assignee IS NOT EMPTY`, `assignee WAS …`) — they keep the value
/// position open rather than reading as a candidate name.
bool IsValuePositionKeyword(const std::string& tokenLowered) {
    return tokenLowered == "in" || tokenLowered == "not" || tokenLowered == "is" || tokenLowered == "was" ||
           tokenLowered == "changed" || tokenLowered == "empty" || tokenLowered == "null";
}

/// Resolve a display name to its account id — but only when the match is UNIQUE across
/// `users` (the same account listed twice, e.g. catalog + search-resolved, still counts as
/// one). Two different accounts sharing the name return "" so an ambiguous name is left as
/// typed instead of silently picking one; a user with an empty display name never matches.
std::string UniqueAccountIdForName(const std::string& nameLowered, const std::vector<TrackerUser>& users) {
    std::string found;
    for (size_t i = 0; i < users.size(); ++i) {
        if (users[i].AccountId.empty() || users[i].DisplayName.empty() ||
            !tracker_query_suggest::AsciiEqualsIgnoreCaseToLowered(users[i].DisplayName, nameLowered)) {
            continue;
        }
        if (found.empty()) {
            found = users[i].AccountId;
        } else if (found != users[i].AccountId) {
            return std::string();
        }
    }
    return found;
}

/// Shared clause-aware walk for BOTH mapping directions: hands exactly the value tokens of
/// user-type fields (per `fields`) to `mapUserValue(token, tokenLowered)`, which returns the
/// full replacement text ("" = leave as typed). Everything else is copied byte-for-byte:
///  - function arguments (`membersOf("…")` — "(" after a bare unresolved token), including
///    parens nested inside them;
///  - the ORDER BY tail (nothing after a bare `order` is a value);
///  - other fields' values, keywords, and cf[…] clauses (`cf` never resolves as a field
///    token, so custom-field values pass through untouched in BOTH directions).
/// Inside an IN-list ("(" after a bare `in`) tokens are values only — field probing is
/// suppressed so `status in ("assignee", …)` cannot re-arm the state. One walker for both
/// directions keeps them exact inverses: a token is only rewritten in a position the
/// opposite direction would rewrite back.
template <typename MapValueFn>
std::string RewriteUserFieldValues(const std::string& query, const std::vector<TrackerField>& fields, int* outReplaced,
                                   const MapValueFn& mapUserValue) {
    std::string out;
    out.reserve(query.size());
    int replaced = 0;
    bool inUserValue = false; // last field token was user-type; its value position is open
    bool orderTail = false;   // saw bare ORDER — sort keys / directions to end of query
    bool pendingFunc = false; // last token was bare + unresolved: a "(" makes it a function
    bool pendingIn = false;   // last token was bare IN: a "(" opens a value list
    std::vector<char> parens; // 'f' = function args, 'l' = IN-list, 'b' = boolean group
    int funcDepth = 0;
    int listDepth = 0;
    ForEachValueToken(
        query,
        [&](const std::string& token, size_t spanStart, size_t spanEnd) {
            const bool quoted = query[spanStart] == '"';
            const std::string tokenLowered = AsciiLowered(token);
            const size_t spanLen = spanEnd - spanStart;
            if (orderTail || funcDepth > 0) {
                pendingFunc = !quoted; // a bare token here can still open a nested function
                pendingIn = false;
                out.append(query, spanStart, spanLen);
                return;
            }
            if (!quoted && IsClauseBreakKeyword(tokenLowered)) {
                inUserValue = false;
                orderTail = tokenLowered == "order";
                pendingFunc = false;
                pendingIn = false;
                out.append(query, spanStart, spanLen);
                return;
            }
            if (!quoted && IsValuePositionKeyword(tokenLowered)) {
                pendingIn = tokenLowered == "in";
                pendingFunc = false;
                out.append(query, spanStart, spanLen);
                return;
            }
            if (inUserValue) {
                const std::string mapped = mapUserValue(token, tokenLowered);
                if (!mapped.empty()) {
                    out.append(mapped);
                    ++replaced;
                    pendingFunc = false;
                    pendingIn = false;
                    return; // replaced; stay in value position (an IN-list continues)
                }
            }
            // Not a mappable value — sloppy input may start the next clause without an
            // explicit AND, and a quoted token can be a quoted field name ("Request
            // participants" = …). Inside an IN-list a token can only be a value.
            if (listDepth == 0) {
                const TrackerField* f = tracker_query_suggest::FindTrackerField(fields, token);
                if (f != nullptr) {
                    inUserValue = tracker_query_suggest::IsQueryUserField(*f);
                    pendingFunc = false;
                    pendingIn = false;
                    out.append(query, spanStart, spanLen);
                    return;
                }
            }
            pendingFunc = !quoted; // an unresolved bare token may be a function name
            pendingIn = false;
            out.append(query, spanStart, spanLen);
        },
        [&](char c) {
            if (c == '(') {
                char kind = 'b';
                if (funcDepth > 0 || pendingFunc) {
                    kind = 'f';
                    ++funcDepth;
                } else if (pendingIn) {
                    kind = 'l';
                    ++listDepth;
                }
                parens.push_back(kind);
                pendingFunc = false;
                pendingIn = false;
            } else if (c == ')') {
                if (!parens.empty()) {
                    const char kind = parens.back();
                    parens.pop_back();
                    if (kind == 'f') {
                        --funcDepth;
                    } else if (kind == 'l') {
                        --listDepth;
                        inUserValue = false; // the list closed the value position
                    }
                }
                pendingFunc = false;
                pendingIn = false;
            } else if (!std::isspace(static_cast<unsigned char>(c))) {
                pendingFunc = false;
                pendingIn = false;
            }
            out.push_back(c);
        });
    if (outReplaced != nullptr) {
        *outReplaced = replaced;
    }
    return out;
}

} // namespace

std::string RenderQueryWithUserNames(const std::string& query, const std::vector<TrackerField>& fields,
                                     const std::vector<TrackerUser>& users, int* outReplaced) {
    if (outReplaced != nullptr) {
        *outReplaced = 0;
    }
    if (query.empty() || users.empty()) {
        return query;
    }
    return RewriteUserFieldValues(query, fields, outReplaced,
                                  [&users](const std::string& token, const std::string& /*tokenLowered*/) {
                                      if (!LooksLikeAccountId(token)) {
                                          return std::string();
                                      }
                                      const std::string name = LookupDisplayName(token, users);
                                      if (name.empty()) {
                                          return std::string();
                                      }
                                      // Round-trip guard: rewrite only when the name maps back to
                                      // exactly this id across `users` — a display name shared by
                                      // two accounts would strand the precise id as an ambiguous
                                      // literal name the inverse refuses at apply.
                                      if (UniqueAccountIdForName(AsciiLowered(name), users) != token) {
                                          return std::string();
                                      }
                                      return tracker_query_suggest::InsertForValueToken(name);
                                  });
}

std::string UnquoteValueToken(const std::string& text) {
    if (text.size() < 2 || text.front() != '"') {
        return text;
    }
    std::string value;
    ScanQuotedValue(text, 0, value);
    return value;
}

std::vector<std::string> CollectUnresolvedAccountIds(const std::string& query,
                                                     const std::vector<TrackerUser>& knownUsers) {
    std::vector<std::string> out;
    if (query.empty()) {
        return out;
    }
    ForEachValueToken(
        query,
        [&](const std::string& token, size_t, size_t) {
            if (!LooksLikeAccountId(token) || !LookupDisplayName(token, knownUsers).empty()) {
                return;
            }
            for (size_t k = 0; k < out.size(); ++k) {
                if (out[k] == token) {
                    return;
                }
            }
            out.push_back(token);
        },
        [](char) {});
    return out;
}

std::string RenderQueryWithAccountIds(const std::string& query, const std::vector<TrackerField>& fields,
                                      const std::vector<TrackerUser>& users, int* outReplaced) {
    if (outReplaced != nullptr) {
        *outReplaced = 0;
    }
    if (query.empty() || users.empty()) {
        return query;
    }
    return RewriteUserFieldValues(query, fields, outReplaced,
                                  [&users](const std::string& token, const std::string& tokenLowered) {
                                      if (LooksLikeAccountId(token)) {
                                          return std::string();
                                      }
                                      const std::string accountId = UniqueAccountIdForName(tokenLowered, users);
                                      if (accountId.empty()) {
                                          return std::string();
                                      }
                                      return tracker_query_suggest::InsertForValueToken(accountId);
                                  });
}

} // namespace jql_user_display
