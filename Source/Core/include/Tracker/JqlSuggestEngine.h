#pragma once

#include "QuerySuggestTypes.h"
#include "TrackerFieldSchema.h"

#include <vector>

/** Build JQL autocomplete suggestions from buffer + cursor. Optionally fills meta for hybrid user search.
 *  `fields` / `users` are the app-owned catalogs (callers holding the full app object pass the
 *  catalog getters' results; the referenced storage outlives any frame). */
void BuildJqlSuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                         const std::vector<TrackerField>& fields, const std::vector<TrackerUser>& users,
                         QuerySuggestBuild& out, QuerySuggestMeta* metaOut = nullptr);
