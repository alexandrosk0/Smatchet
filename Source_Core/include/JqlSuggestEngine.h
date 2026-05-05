#pragma once

#include "QuerySuggestTypes.h"

class AppController;

/** Build JQL autocomplete suggestions from buffer + cursor. Optionally fills meta for hybrid user search. */
void BuildJqlSuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd, const AppController& app,
                         QuerySuggestBuild& out, QuerySuggestMeta* metaOut = nullptr);
