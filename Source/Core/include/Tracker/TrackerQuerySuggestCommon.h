#pragma once

#include "QuerySuggestTypes.h"
#include "Tracker/TrackerFieldSchema.h"

#include <string>
#include <unordered_set>
#include <vector>

/// Backend-agnostic query-suggest helpers shared by the JQL (Jira) and Plane
/// filter autocomplete engines. These are pure string / prefix-match / field-catalog
/// utilities with NO backend-specific grammar — the per-backend operator/keyword
/// tables, value-context parsing, and Suggest() entry points stay in each engine.
/// Shared *implementation*, not a shared *interface*: it never touches
/// ITrackerBackend or its role interfaces (no backend-leak invariant violation).
namespace tracker_query_suggest {

/// True for characters that are part of an unquoted identifier / value token
/// (alphanumeric, '_', '.', '-').
bool IsQueryIdChar(unsigned char ch);

/// True when the value must be wrapped in quotes to be a valid token (empty,
/// or containing a non-id char, a quote, or a backslash).
bool QueryValueNeedsQuotes(const std::string& s);

/// Wrap a value in double-quotes, backslash-escaping embedded quotes / backslashes.
std::string QueryQuotedValue(const std::string& s);

/// Return the value as it should be inserted: quoted iff it needs quoting.
std::string InsertForValueToken(const std::string& raw);

/// Scan [0, min(bufLen, cursor)) tracking double-quote string state, honouring
/// backslash escapes. Reports whether the cursor sits inside an open string and
/// the index of that string's opening quote.
void ScanStringStateToCursor(const char* buf, int bufLen, int cursor, bool& outInString, int& outStringOpenIndex);

/// Case-insensitive equality where `alreadyLowered` is already lower-cased ASCII.
bool AsciiEqualsIgnoreCaseToLowered(const std::string& value, const std::string& alreadyLowered);

/// Case-insensitive prefix test; `prefixLower` is already lower-cased ASCII.
bool AsciiStartsWithIgnoreCase(const std::string& value, const std::string& prefixLower);

/// Resolve a field by id (then by name) against the available-field catalog,
/// case-insensitively. Returns nullptr when no match. Takes the field vector
/// directly (decoupled from AppController for testability).
const TrackerField* FindTrackerField(const std::vector<TrackerField>& fields, const std::string& token);

/// Append a suggestion if its insert text is non-empty and not already seen.
/// An empty label falls back to the insert text.
void AddSuggestionUnique(std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen, std::string label,
                         std::string insert);

/// Append each term in `terms` whose text prefix-matches `prefix` (case-insensitive).
void AppendTerms(const std::string& prefix, const char* const* terms, int termCount, std::vector<QuerySuggestion>& out,
                 std::unordered_set<std::string>& seen);

/// Append field-id / field-name suggestions from the catalog that prefix-match.
/// Takes the field vector directly (decoupled from AppController for testability).
void AppendFieldCatalog(const std::vector<TrackerField>& fields, const std::string& prefix,
                        std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen);

/// True for user-type fields (single or multi).
bool IsQueryUserField(const TrackerField& field);

/// True for date / date-time fields.
bool IsQueryDateField(const TrackerField& field);

/// Shared entry prologue for a suggest pass: resets `metaOut` (when non-null), clears `out`,
/// and clamps cursor/selection into [0, bufLen]. Returns false on a null buffer — the caller
/// returns immediately with the cleared (empty) build.
bool BeginSuggestBuild(const char* buf, int bufLen, int& cursor, int& selStart, int& selEnd, QuerySuggestBuild& out,
                       QuerySuggestMeta* metaOut);

/// Resolve the [ReplaceStart, ReplaceEnd) span into `out` and return the prefix the
/// suggestions replace. Three-way: active selection wins; else an open string literal spans
/// from its opening quote to the cursor; else the identifier run straddling the cursor.
std::string ResolveQueryReplaceRange(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                     QuerySuggestBuild& out);

/// Shared exit epilogue: case-insensitive label sort (ties break shorter-first) and the
/// popup-size cap (80 items) so the suggestion list stays scannable and cheap to render.
void SortAndCapSuggestions(std::vector<QuerySuggestion>& items);

} // namespace tracker_query_suggest
