#pragma once

// Structural check of a typed filter query before it replaces a view's query (INPUT_UX_AUDIT.md #63): an
// unbalanced parenthesis or quote, a query that ends on an operator or keyword, and (for JQL, when the
// field catalog is known) a field name the catalog does not have. It is a typing aid, not a JQL parser:
// the tracker stays the judge of what a query means. Pure: no I/O, no ImGui.

#include "TrackerFieldSchema.h"
#include "TrackerFieldValueParseExplain.h"

#include <string>
#include <vector>

namespace JqlQueryValidate {

enum class QueryDialect : unsigned char {
    Jql,        ///< Jira: field names are checked against the catalog
    FieldValue, ///< Plane / GitHub / Linear "field:value" filters: structure only
};

/// True for an error the query cannot be sent with (unbalanced parenthesis or quote, a dangling
/// operator). An unknown field is a warning: the catalog may be stale and the tracker may still know it.
bool IsBlockingQueryError(const TrackerParseExplain::ParseMessage& msg);

/// The first problem in `text`, or a message with ParseMessageId::None when there is none. `fields` may be
/// null or empty, which skips the field-name check. `At`/`Len` point at the offending token.
TrackerParseExplain::ParseMessage ValidateQuery(const std::string& text, QueryDialect dialect,
                                                const std::vector<TrackerField>* fields);

} // namespace JqlQueryValidate
