#pragma once

#include "QuerySuggestTypes.h"

class AppController;

/**
 * Plane filter mini-language (client-side string stored in view Jql slot):
 *   fieldId:value  AND  fieldId:value
 * Colon separates field from value; AND/OR join clauses (case-insensitive).
 * Value token uses same quoting rules as JQL for non-id characters (engine mirrors JQL value insert style).
 */
void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                const AppController& app, QuerySuggestBuild& out, QuerySuggestMeta* metaOut = nullptr);
