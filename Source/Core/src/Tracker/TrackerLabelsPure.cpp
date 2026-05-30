#include "TrackerLabelsPure.h"

#include "StringUtil.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace TrackerLabelsPure {

std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            const std::string trimmed = TrimCopy(current);
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    const std::string trimmed = TrimCopy(current);
    if (!trimmed.empty()) {
        result.push_back(trimmed);
    }
    return result;
}

bool LabelsEqualCaseInsensitive(const std::string& a, const std::string& b) {
    return ToLowerAsciiCopy(a) == ToLowerAsciiCopy(b);
}

bool ContainsLabelCaseInsensitive(const std::vector<std::string>& values, const std::string& needle) {
    return std::any_of(values.begin(), values.end(),
                       [&](const std::string& value) { return LabelsEqualCaseInsensitive(value, needle); });
}

std::vector<std::string> SortAndUniqueLabels(std::vector<std::string> values) {
    std::vector<std::string> trimmedValues;
    trimmedValues.reserve(values.size());
    for (const auto& value : values) {
        const std::string trimmed = TrimCopy(value);
        if (!trimmed.empty()) {
            trimmedValues.push_back(trimmed);
        }
    }
    values = std::move(trimmedValues);
    std::sort(values.begin(), values.end(), [](const std::string& a, const std::string& b) {
        const std::string lowerA = ToLowerAsciiCopy(a);
        const std::string lowerB = ToLowerAsciiCopy(b);
        if (lowerA != lowerB) {
            return lowerA < lowerB;
        }
        return a < b;
    });
    values.erase(
        std::unique(values.begin(), values.end(),
                    [](const std::string& a, const std::string& b) { return LabelsEqualCaseInsensitive(a, b); }),
        values.end());
    return values;
}

bool LabelMatchesFilter(const std::string& label, const std::string& filterLower) {
    if (filterLower.empty()) {
        return true;
    }
    return ToLowerAsciiCopy(label).find(filterLower) != std::string::npos;
}

std::vector<std::string> FilterSuggestionsForDisplay(const std::vector<std::string>& suggestions,
                                                     const std::vector<std::string>& selectedLabels,
                                                     const std::string& filterLower) {
    std::vector<std::string> out;
    out.reserve(suggestions.size());
    std::copy_if(suggestions.begin(), suggestions.end(), std::back_inserter(out), [&](const std::string& suggestion) {
        return ContainsLabelCaseInsensitive(selectedLabels, suggestion) || LabelMatchesFilter(suggestion, filterLower);
    });
    return out;
}

} // namespace TrackerLabelsPure
