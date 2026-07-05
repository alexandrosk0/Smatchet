#pragma once

#include "QuerySuggestTypes.h"
#include "TrackerFieldSchema.h"

#include <vector>

/**
 * Pure core of the Plane filter-query autocomplete engine (gap map Tier 1 #5). Byte-identical
 * lift of PlaneQuerySuggestEngine.cpp; the only change is the dependency seam — the one
 * AppController getter it consumed (`GetAvailableFields()`) becomes an explicit vector
 * parameter. The public `BuildPlaneQuerySuggestions(..., const AppController&, ...)` entry
 * point survives as a thin forwarding shell in PlaneQuerySuggestEngine.cpp.
 *
 * Owner: `tracker-backend` subsystem. Test surface:
 * tests/Core/PlaneQuerySuggestEnginePure.test.cpp.
 */
void BuildPlaneQuerySuggestionsPure(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                    const std::vector<TrackerField>& fields, QuerySuggestBuild& out,
                                    QuerySuggestMeta* metaOut = nullptr);
