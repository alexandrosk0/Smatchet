#pragma once

#include <string>

namespace JqlProjectScope {

// ExtractSingleProject returns "" when zero or more-than-one project is referenced
// (sentinel for the "ambiguous / multi-project" case — callers must surface a picker).
std::string ExtractSingleProject(const std::string& jql);

bool HasProjectScope(const std::string& jql);

} // namespace JqlProjectScope
