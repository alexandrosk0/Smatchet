#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib> // std::strtoll (CPP_CODE_AUDIT.md #33f — was only transitively included)
#include <string>
#include <vector>

inline std::string TrimCopyAsciiWhitespace(const std::string& input) {
    size_t start = 0;
    size_t end = input.size();
    while (start < end &&
           (input[start] == ' ' || input[start] == '\t' || input[start] == '\n' || input[start] == '\r')) {
        ++start;
    }
    while (end > start &&
           (input[end - 1] == ' ' || input[end - 1] == '\t' || input[end - 1] == '\n' || input[end - 1] == '\r')) {
        --end;
    }
    return input.substr(start, end - start);
}

inline std::string TrimCopy(const std::string& input) { return TrimCopyAsciiWhitespace(input); }

inline std::string ToLowerAsciiCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline std::string ToUpperAsciiCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

inline std::string JoinStrings(const std::vector<std::string>& items, const std::string& separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += items[i];
    }
    return out;
}

// File NAME component of a path (C++14 — no std::filesystem). For user-facing error
// text: a full path in a toast leaks the local filesystem layout into screenshots.
inline std::string FileNameOfPath(const std::string& path) {
    const std::size_t sep = path.find_last_of("/\\");
    return sep == std::string::npos ? path : path.substr(sep + 1);
}

inline std::string TruncateForLog(const std::string& input, size_t maxLen = 600) {
    if (input.size() <= maxLen) {
        return input;
    }
    return input.substr(0, maxLen) + "... [truncated]";
}

inline bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char ch1, char ch2) {
        return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2));
    });
    return it != haystack.end();
}

/** Natural Jira issue key comparison: "PROJ-2" < "PROJ-10" (lexicographic prefix, then numeric suffix). */
inline bool CompareIssueKeyNatural(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) -> std::pair<std::string, long long> {
        const size_t dash = s.rfind('-');
        if (dash != std::string::npos && dash + 1 < s.size()) {
            std::string project = s.substr(0, dash);
            const std::string tail = s.substr(dash + 1);
            if (!tail.empty()) {
                char* end = nullptr;
                const long long v = std::strtoll(tail.c_str(), &end, 10);
                if (end == tail.c_str() + tail.size()) {
                    return {std::move(project), v};
                }
            }
        }
        return {s, 0};
    };
    const auto pa = split(a);
    const auto pb = split(b);
    if (pa.first != pb.first)
        return pa.first < pb.first;
    return pa.second < pb.second;
}

/// Canonicalize a grid field id, folding legacy aliases onto their unified id.
/// Jira once exposed a singular `comment` column (raw ADF blob) before the unified
/// `comments` count/modal cell (#1291); a view saved in that era still carries the
/// legacy id. Fold it here so the old column renders the unified cell and dedups
/// against an explicit `comments` column. Backend-safe: GitHub/Plane never use
/// `comment`. Any other id passes through unchanged.
inline std::string CanonicalizeGridFieldId(const std::string& fieldId) {
    if (fieldId == "comment") {
        return "comments";
    }
    return fieldId;
}

/// Canonicalize a full grid column key the same way TicketGridColumnsBuilder builds a
/// column's Key from view.Fields, so a saved view.ColumnOrder entry matches by key
/// regardless of stray whitespace or a legacy field alias. A "field:<id>" key folds its
/// id via CanonicalizeGridFieldId after an ASCII-whitespace trim (mirroring the Fields
/// loop, which keys on "field:" + CanonicalizeGridFieldId(TrimCopyAsciiWhitespace(id)));
/// any other key (e.g. "id") is just trimmed and passed through. Keeps the ColumnOrder
/// lookup in lock-step with the Key so a pre-canonicalization view keeps its column
/// positions instead of dropping them to the appended tail (ticketgrid-columnorder-canon).
inline std::string CanonicalGridColumnKey(const std::string& rawKey) {
    const std::string trimmed = TrimCopyAsciiWhitespace(rawKey);
    if (trimmed.compare(0, 6, "field:") == 0) {
        return "field:" + CanonicalizeGridFieldId(TrimCopyAsciiWhitespace(trimmed.substr(6)));
    }
    return trimmed;
}

/** Split a string by a delimiter, trimming whitespace from parts. */
inline std::vector<std::string> SplitAndTrim(const std::string& input, char delimiter = ',') {
    std::vector<std::string> result;
    std::string current;
    auto flush = [&]() {
        std::string trimmed = TrimCopy(current);
        if (!trimmed.empty())
            result.push_back(trimmed);
        current.clear();
    };
    for (char ch : input) {
        if (ch == delimiter)
            flush();
        else
            current.push_back(ch);
    }
    flush();
    return result;
}

/** Replace tabs and newlines with spaces for spreadsheet-safe copy-paste. */
inline std::string SanitizeForSpreadsheet(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\t' || ch == '\n' || ch == '\r')
            out.push_back(' ');
        else
            out.push_back(ch);
    }
    return out;
}

/// Base64 encode (RFC 4648, standard alphabet, '=' padded). Single source for the repo's
/// base64 — the tracker Basic-auth header, the MCP envelope path, and the bug-report
/// attachment encoder all route here; each previously carried its own copy.
inline std::string Base64Encode(const std::string& in) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned char a = static_cast<unsigned char>(in[i]);
        const unsigned char b = (i + 1 < in.size()) ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const unsigned char c = (i + 2 < in.size()) ? static_cast<unsigned char>(in[i + 2]) : 0u;
        out += table[a >> 2];
        out += table[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < in.size()) ? table[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < in.size()) ? table[c & 63] : '=';
    }
    return out;
}

/// Strip a leading UTF-8 BOM (EF BB BF) if present. Some HTTP servers prepend it to JSON bodies.
inline std::string StripUtf8BomCopy(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEFu && static_cast<unsigned char>(s[1]) == 0xBBu &&
        static_cast<unsigned char>(s[2]) == 0xBFu) {
        s.erase(0, 3);
    }
    return s;
}
