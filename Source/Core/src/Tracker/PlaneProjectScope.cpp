#include "PlaneProjectScope.h"

#include <nlohmann/json.hpp>

namespace PlaneProjectScope {

std::string SetProjectInQuery(const std::string& queryJson, const std::string& projectId) {
    if (projectId.empty()) {
        return queryJson;
    }
    nlohmann::json j = nlohmann::json::object();
    if (!queryJson.empty()) {
        const nlohmann::json parsed = nlohmann::json::parse(queryJson, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            j = parsed;
        }
    }
    j["project_id"] = projectId;
    return j.dump();
}

} // namespace PlaneProjectScope
