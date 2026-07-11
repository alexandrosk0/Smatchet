#pragma once

#include "QuerySuggestTypes.h"
#include "TrackerFieldSchema.h"

#include <vector>

/**
 * Plane filter mini-language (client-side string stored in view Jql slot):
 *   fieldId:value  AND  fieldId:value
 * Colon separates field from value; AND/OR join clauses (case-insensitive).
 * Value token uses same quoting rules as JQL for non-id characters (engine mirrors JQL value insert style).
 * `fields` is the app-owned field catalog (callers holding the full app object pass the catalog
 * getter's result; the referenced storage outlives any frame).
 */
void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                const std::vector<TrackerField>& fields, QuerySuggestBuild& out,
                                QuerySuggestMeta* metaOut = nullptr);
