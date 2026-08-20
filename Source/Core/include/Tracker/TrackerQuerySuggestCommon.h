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

/// Case-insensitive substring test; `needleLower` is already lower-cased ASCII.
/// An empty needle matches everything (same convention as AsciiStartsWithIgnoreCase).
bool AsciiContainsIgnoreCase(const std::string& value, const std::string& needleLower);

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

/// Append value suggestions for `field` whose raw value or display label prefix-matches
/// `prefix`, de-duplicated through `seen`. Walks AllowedValueOptions (user fields emit an
/// account-id entry plus a display-name entry; others emit value and, when distinct,
/// id-qualified-by-value) then the flat AllowedValues list.
/// `userDisplaySuffix` is the only backend-local divergence — Jira renders " (display name) -> ",
/// Plane " (display) -> "; passing it in keeps both wordings while the body is single-sourced.
void AppendValueSuggestions(const TrackerField& field, const std::string& prefix, const char* userDisplaySuffix,
                            std::vector<QuerySuggestion>& out, std::unordered_set<std::string>& seen);

/// True for date / date-time fields.
bool IsQueryDateField(const TrackerField& field);

/// Resolve the [replaceStart, replaceEnd) span and the prefix the suggestions replace.
/// Three-way branch shared verbatim by both engines: active selection, open-string token,
/// or the identifier run straddling the cursor.
void ResolveQueryReplaceRange(const char* buf, int bufLen, int cursor, int selStart, int selEnd, int& replaceStart,
                              int& replaceEnd, std::string& prefix);

/// Final presentation pass shared by both engines: case-insensitive label sort, then cap at
/// 80 items so the popup stays bounded.
void SortAndCapQuerySuggestions(std::vector<QuerySuggestion>& items);

/// Shared entry prologue for both engines: reset `out` + `metaOut`, reject a null buffer,
/// clamp cursor/selection into [0, bufLen], resolve the replace span + prefix, and publish
/// the span onto `out`. Returns false (outputs already reset) when `buf` is null.
bool BeginQuerySuggestPass(const char* buf, int bufLen, int cursor, int selStart, int selEnd, QuerySuggestBuild& out,
                           QuerySuggestMeta* metaOut, int& replaceStart, int& replaceEnd, std::string& prefix);

} // namespace tracker_query_suggest
