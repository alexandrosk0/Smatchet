#pragma once

#include <algorithm>
#include <cctype>
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

inline std::string TruncateForLog(const std::string& input, size_t maxLen = 600) {
    if (input.size() <= maxLen) {
        return input;
    }
    return input.substr(0, maxLen) + "... [truncated]";
}
