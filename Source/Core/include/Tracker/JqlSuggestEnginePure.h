#pragma once

#include "QuerySuggestTypes.h"

#include <vector>

struct TrackerField;
struct TrackerUser;

/**
 * AppController-free core of the JQL autocomplete engine (JqlSuggestEngine.cpp is the thin
 * shell that feeds it the live catalog). Reparametrized to the field/user vectors — the same
 * seam TrackerQuerySuggestCommon already uses — so the tokenizer, suggest-mode resolver, and
 * per-mode appenders are bucket-A-testable without standing up AppController.
 *
 * Runs synchronously on the UI thread for every keystroke in the views-editor filter input,
 * over tracker-supplied catalog values — it must stay allocation-light and terminate on any
 * buffer/cursor combination.
 *
 * Owner: JqlSuggestEngine (tracker query autocomplete) — pure core lives here.
 * Test surface: tests/Core/JqlSuggestEnginePure.test.cpp.
 */
namespace JqlSuggestEnginePure {

/** Build JQL autocomplete suggestions from buffer + cursor against the given catalog. */
void BuildJqlSuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                         const std::vector<TrackerField>& fields, const std::vector<TrackerUser>& users,
                         QuerySuggestBuild& out, QuerySuggestMeta* metaOut = nullptr);

} // namespace JqlSuggestEnginePure
