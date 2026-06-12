#pragma once

#include <string>

namespace PlaneProjectScope {

/** Inject or replace `project_id` in a Plane structured-query JSON blob. Returns compact JSON.
 *  Non-JSON / empty input becomes `{"project_id":"<projectId>"}`. Empty projectId is a no-op. */
std::string SetProjectInQuery(const std::string& queryJson, const std::string& projectId);

} // namespace PlaneProjectScope
