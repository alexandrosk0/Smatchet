// Pure, ImGui-free decision helpers for the Backend Audit window. Extracted from
// SmatchetUI::drawAuditWindow so the filter / pagination logic is bucket-A testable in the
// doctest rig (no ImGui / AppController link) and the draw body stays under the branch cap.

#include "Ui/SmatchetAudit_detail.h"

#include <cstddef>
#include <string>
#include <vector>

namespace SmatchetAudit {
namespace detail {

std::string JsonStringValue(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) {
        return std::string();
    }
    const auto& v = j[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<unsigned long long>());
    }
    return v.dump();
}

bool ContainsNoCasePreLower(const std::string& haystackLower, const std::string& needleLower) {
    if (needleLower.empty()) {
        return true;
    }
    return haystackLower.find(needleLower) != std::string::npos;
}

bool ActionMatchesFilter(const std::string& action, int filter) {
    switch (filter) {
    case 1:
        return action.find("create") != std::string::npos && action.find("offline") == std::string::npos;
    case 2:
        return action.find("update") != std::string::npos || action.find("transition") != std::string::npos ||
               action.find("field_edit") != std::string::npos;
    case 3:
        return action.find("comment") != std::string::npos;
    case 4:
        return action.find("attach") != std::string::npos;
    case 5:
        return action.find("offline") != std::string::npos;
    default:
        return true;
    }
}

bool ResultMatchesFilter(bool success, int filter) {
    if (filter == 1)
        return success;
    if (filter == 2)
        return !success;
    return true;
}

int ClampRowsPerPage(int value) {
    if (value < 25)
        return 25;
    if (value > 1000)
        return 1000;
    return value;
}

std::vector<std::size_t> CollectFilteredAuditIndices(const std::vector<nlohmann::json>& events,
                                                     const std::vector<std::string>& searchLower, int actionFilter,
                                                     int resultFilter, const std::string& queryLower,
                                                     bool newestFirst) {
    std::vector<std::size_t> out;
    out.reserve(events.size());
    const auto keep = [&](std::size_t i) {
        const nlohmann::json& e = events[i];
        const std::string action = JsonStringValue(e, "action");
        const bool success = e.value("success", false);
        if (!ActionMatchesFilter(action, actionFilter) || !ResultMatchesFilter(success, resultFilter)) {
            return;
        }
        if (i >= searchLower.size() || !ContainsNoCasePreLower(searchLower[i], queryLower)) {
            return;
        }
        out.push_back(i);
    };
    if (newestFirst) {
        for (std::size_t i = events.size(); i > 0; --i) {
            keep(i - 1);
        }
    } else {
        for (std::size_t i = 0; i < events.size(); ++i) {
            keep(i);
        }
    }
    return out;
}

int ComputeAuditPageCount(std::size_t filteredCount, int rowsPerPage) {
    if (filteredCount == 0 || rowsPerPage <= 0) {
        return 1;
    }
    const std::size_t per = static_cast<std::size_t>(rowsPerPage);
    return static_cast<int>((filteredCount + per - 1) / per);
}

} // namespace detail
} // namespace SmatchetAudit
