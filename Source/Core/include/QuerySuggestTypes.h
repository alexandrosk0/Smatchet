#pragma once

#include <string>
#include <vector>

/** One row in query autocomplete (JQL or Plane filter). */
struct QuerySuggestion {
    std::string Label;
    std::string Insert;
    /// Insert is a whole query: picking the row replaces the whole input with it and applies it.
    bool CommitOnPick = false;
};

/** Output of a synchronous suggest pass: replace range + ranked items. */
struct QuerySuggestBuild {
    int ReplaceStart = 0;
    int ReplaceEnd = 0;
    std::vector<QuerySuggestion> Items;
};

/** Hints for UI (debounced user search, footer). Filled by engines when non-null. */
struct QuerySuggestMeta {
    /** True when cursor is in a user-type field value token (Jira: may merge live user search). */
    bool UserValueToken = false;
    /** Prefix inside replace span (search query for users). */
    std::string UserSearchPrefix;
};
