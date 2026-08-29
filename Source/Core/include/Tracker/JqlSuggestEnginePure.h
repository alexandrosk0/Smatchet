#pragma once

#include "QuerySuggestTypes.h"
#include "TrackerFieldSchema.h"

#include <vector>

/**
 * Pure core of the JQL autocomplete engine (gap map Tier 1 #5). Byte-identical lift of
 * JqlSuggestEngine.cpp's entire tokenizer / mode-resolver / suggestion pipeline; the only
 * change is the dependency seam — the two AppController getters the engine consumed
 * (`GetAvailableFields()` / `GetAvailableUsers()`) become explicit vector parameters, so the
 * highest-frequency untrusted-input parser in the app (it runs on every omnibox keystroke)
 * compiles into the test rigs without AppController/ImGui. The public
 * `BuildJqlSuggestions(..., const AppController&, ...)` entry point survives as a thin
 * forwarding shell in JqlSuggestEngine.cpp.
 *
 * Owner: `tracker-backend` subsystem. Test surface: tests/Core/JqlSuggestEnginePure.test.cpp.
 */
void BuildJqlSuggestionsPure(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                             const std::vector<TrackerField>& fields, const std::vector<TrackerUser>& users,
                             QuerySuggestBuild& out, QuerySuggestMeta* metaOut = nullptr);

/// Build the JQL value-token for a user: the display name (else accountId), always
/// double-quoted, JQL-escaped through tracker_jql::QuoteLiteral (security: H3 + E1).
/// Shared by the catalog suggestions and the async user-search rows so both insert
/// byte-identical text and dedup-by-insert holds across the two sources.
std::string BuildJqlUserInsert(const TrackerUser& user);
