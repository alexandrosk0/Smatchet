#pragma once

#include <string>

namespace tracker_jql {

// Escape `\` and `"` in `value` for safe insertion inside a JQL double-quoted literal.
// Any value from a foreign trust boundary (a tracker-server-supplied issue key,
// AccountId, or display name) must pass through here, or a `"` breaks out of the literal
// and lets the server rewrite the query (security: H3 + E1). Backslash is escaped first
// (`\` -> `\\`) then the quote (`"` -> `\"`). Returns the escaped INNER content only —
// the caller adds the surrounding quotes. Jira-JQL-specific grammar, so it lives behind
// the per-backend Tracker include tree, never on ITrackerBackend or a role interface.
std::string QuoteLiteral(const std::string& value);

} // namespace tracker_jql
