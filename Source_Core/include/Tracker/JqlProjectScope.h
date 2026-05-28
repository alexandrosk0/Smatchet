#pragma once

#include <string>

namespace JqlProjectScope {

// ExtractSingleProject returns "" when zero or more-than-one project is referenced
// (sentinel for the "ambiguous / multi-project" case — callers must surface a picker).
std::string ExtractSingleProject(const std::string& jql);

bool HasProjectScope(const std::string& jql);

// Replaces the first existing project clause (`project = X` or `project in (...)`) with
// `project = <projectKey>`. If no project clause exists, prepends `project = <projectKey> AND `
// to the JQL. Returns the modified JQL. If `projectKey` is empty, returns `jql` unchanged.
std::string SetProjectClause(const std::string& jql, const std::string& projectKey);

} // namespace JqlProjectScope
