#pragma once

#include "QuerySuggestTypes.h"

#include <vector>

struct TrackerField;

/**
 * AppController-free core of the Plane filter-mini-language autocomplete engine
 * (PlaneQuerySuggestEngine.cpp is the thin shell that feeds it the live catalog).
 * Reparametrized to the field vector — the same seam TrackerQuerySuggestCommon uses — so the
 * `fieldId:value` context parse and the field/value appenders are bucket-A-testable without
 * standing up AppController. Runs synchronously on the UI thread per keystroke.
 *
 * Owner: PlaneQuerySuggestEngine (tracker query autocomplete) — pure core lives here.
 * Test surface: tests/Core/PlaneQuerySuggestEnginePure.test.cpp.
 */
namespace PlaneQuerySuggestEnginePure {

/** Build Plane filter autocomplete suggestions from buffer + cursor against the given catalog. */
void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                const std::vector<TrackerField>& fields, QuerySuggestBuild& out,
                                QuerySuggestMeta* metaOut = nullptr);

} // namespace PlaneQuerySuggestEnginePure
