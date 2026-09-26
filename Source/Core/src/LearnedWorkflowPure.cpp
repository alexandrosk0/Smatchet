#include "LearnedWorkflowPure.h"

#include "Json/BoundedJsonParse.h"
#include "StringUtil.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace smatchet::workflow {

const char* kLearnedTransitionsKind = "workflow_transitions";

std::string BuildLearnedTransitionsKey(const std::string& projectKey, const std::string& issueTypeKey,
                                       const std::string& fromStatusKey) {
    if (projectKey.empty() || issueTypeKey.empty() || fromStatusKey.empty()) {
        return "";
    }
    return projectKey + "|" + issueTypeKey + "|" + fromStatusKey;
}

std::string SerializeTransitionTargets(const std::vector<TrackerFieldOption>& options) {
    json arr = json::array();
    for (const auto& opt : options) {
        json obj;
        obj["id"] = opt.Id;
        obj["name"] = opt.Value;
        arr.push_back(obj);
    }
    return arr.dump();
}

bool ParseTransitionTargets(const std::string& jsonStr, std::vector<TrackerFieldOption>& out) {
    std::string errOut;
    const auto parsed = json_safe::ParseBounded(jsonStr, errOut);
    if (!errOut.empty() || !parsed.is_array()) {
        return false;
    }
    out.clear();
    for (const auto& item : parsed) {
        if (item.is_object() && item.contains("id") && item.contains("name")) {
            TrackerFieldOption opt;
            opt.Id = item["id"].is_string() ? item["id"].get<std::string>() : "";
            opt.Value = item["name"].is_string() ? item["name"].get<std::string>() : "";
            if (!opt.Id.empty() || !opt.Value.empty()) {
                out.push_back(opt);
            }
        }
    }
    return true;
}

} // namespace smatchet::workflow
