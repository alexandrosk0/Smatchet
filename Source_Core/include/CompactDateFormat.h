#pragma once

#include <string>

/** Compact display for Jira ISO date/datetime strings (grid/tooltips use raw elsewhere). */
std::string FormatCompactJiraDateForDisplay(const std::string& raw);
