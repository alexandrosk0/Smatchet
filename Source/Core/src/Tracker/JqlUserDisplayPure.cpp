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

/// The 24-hex object id older / non-Cloud deployments emit as an account key.
bool IsHex24(const std::string& s) { return s.size() == 24 && IsAllHex(s, 0, 24); }

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

} // namespace

bool LooksLikeAccountId(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    if (IsUuid(token) || IsHex24(token)) {
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

std::string RenderQueryWithUserNames(const std::string& query, const std::vector<TrackerUser>& users,
                                     int* outReplaced) {
    if (outReplaced != nullptr) {
        *outReplaced = 0;
    }
    if (query.empty() || users.empty()) {
        return query;
    }
    std::string out;
    out.reserve(query.size());
    std::string token;
    int replaced = 0;
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
            out.push_back(query[i]);
            i = next;
            continue;
        }
        std::string name;
        if (LooksLikeAccountId(token)) {
            name = LookupDisplayName(token, users);
        }
        if (name.empty()) {
            out.append(query, i, next - i);
        } else {
            out.append(tracker_query_suggest::InsertForValueToken(name));
            ++replaced;
        }
        i = next;
    }
    if (outReplaced != nullptr) {
        *outReplaced = replaced;
    }
    return out;
}

} // namespace jql_user_display
