#pragma once

#include "Views.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

class AppController;
struct UiDrawSession;

namespace SmatchetViewsDashboardUiDetail {

inline std::string JoinCsvLocal(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0)
            out += ", ";
        out += values[i];
    }
    return out;
}

inline std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            size_t start = 0;
            size_t end = current.size();
            while (start < end && (current[start] == ' ' || current[start] == '\t'))
                ++start;
            while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t'))
                --end;
            if (end > start) {
                result.push_back(current.substr(start, end - start));
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    size_t start = 0;
    size_t end = current.size();
    while (start < end && (current[start] == ' ' || current[start] == '\t'))
        ++start;
    while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t'))
        --end;
    if (end > start) {
        result.push_back(current.substr(start, end - start));
    }
    return result;
}

template <size_t N> inline void CopyStringToBuffer(char (&dst)[N], const std::string& str) {
    static_assert(N > 0, "CopyStringToBuffer requires a non-empty char array");
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(str.size(), cap);
    if (n > 0)
        std::memcpy(dst, str.data(), n);
}

inline bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty())
        return true;
    auto toLower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string loweredText(text.size(), '\0');
    std::string loweredNeedle(needle.size(), '\0');
    std::transform(text.begin(), text.end(), loweredText.begin(), toLower);
    std::transform(needle.begin(), needle.end(), loweredNeedle.begin(), toLower);
    return loweredText.find(loweredNeedle) != std::string::npos;
}

inline std::vector<std::string> ToSortedVector(const std::unordered_set<std::string>& values) {
    std::vector<std::string> result(values.begin(), values.end());
    std::sort(result.begin(), result.end());
    return result;
}

void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory);

void ApplyViewsActiveJqlFromBuffers(AppController& app, UiDrawSession& d, Views& viewState,
                                   const ViewDefinition& activeView);

void DrawJqlQueryEditor(AppController& app, UiDrawSession& d, Views& viewState, const ViewDefinition& activeView);

} // namespace SmatchetViewsDashboardUiDetail
