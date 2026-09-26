#include "LearnedWorkflowPure.h"

#include "Json/BoundedJsonParse.h"
#include "Tracker/TrackerFieldValueParser.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace workflow {

std::string BuildLearnedTransitionsKey(const std::string& projectKey, const std::string& issueTypeKey,
                                       const std::string& fromStatusKey) {
    if (projectKey.empty() || issueTypeKey.empty() || fromStatusKey.empty()) {
        return std::string();
    }
    return projectKey + "|" + issueTypeKey + "|" + fromStatusKey;
}

std::string SerializeTransitionTargets(const std::vector<TrackerFieldOption>& options) {
    nlohmann::json arr = nlohmann::json::array();
    for (const TrackerFieldOption& opt : options) {
        nlohmann::json entry = nlohmann::json::object();
        entry["id"] = opt.Id;
        entry["name"] = opt.Value;
        arr.push_back(std::move(entry));
    }
    // A status name is tracker-supplied text; replace invalid UTF-8 instead of throwing.
    return arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool ParseTransitionTargets(const std::string& json, std::vector<TrackerFieldOption>& out) {
    out.clear();
    std::string err;
    const nlohmann::json parsed = smatchet::json_safe::ParseBounded(json, err);
    if (!err.empty() || !parsed.is_array()) {
        return false;
    }
    for (const nlohmann::json& entry : parsed) {
        if (!entry.is_object()) {
            continue;
        }
        TrackerFieldOption opt;
        const auto id = entry.find("id");
        if (id != entry.end()) {
            opt.Id = JsonIdToString(*id); // a string or integer id; anything else reads as absent
        }
        const auto name = entry.find("name");
        if (name != entry.end() && name->is_string()) {
            opt.Value = name->get<std::string>();
        }
        if (opt.Id.empty() && opt.Value.empty()) {
            continue;
        }
        out.push_back(std::move(opt));
    }
    return true;
}

} // namespace workflow
} // namespace smatchet
