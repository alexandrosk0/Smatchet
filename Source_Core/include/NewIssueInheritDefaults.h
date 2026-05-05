#pragma once

#include <string>
#include <vector>

/** Single source for config defaults and IssueDraftHelpers::DefaultNewIssueInheritFieldIds(). */
inline std::vector<std::string> DefaultNewIssueInheritFieldIdsList() {
    return {"description", "priority", "assignee", "labels", "components"};
}






