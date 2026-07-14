#ifndef SMATCHET_TRACKER_FIELD_PAYLOAD_PURE_H
#define SMATCHET_TRACKER_FIELD_PAYLOAD_PURE_H

#include "SmatchetResult.h"
#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/**
 * Pure per-family payload builders for Jira REST `fields` map construction.
 *
 * Lifted from TrackerFieldPayload.cpp to break the transitive include chain
 * (TrackerFieldPayload.h -> JiraClient.h -> ITrackerBackend.h -> LocalCacheManager.h
 * -> SQLite/cpr) so the per-family logic can be unit-tested under the doctest
 * rig without bringing the HTTP / cache / config stack along.
 *
 * Every function here is a pure data-shape transform: takes PODs from
 * TrackerFieldSchema.h (TrackerField / TrackerFieldOption) plus raw strings,
 * returns nlohmann::json. No HTTP, no SQLite, no ImGui, no JiraClient.
 *
 * TrackerFieldPayload.cpp (the production facade) delegates to these so the
 * non-pure callers (issue-create pipeline, edit pipeline) keep their existing
 * include path through TrackerFieldPayload.h.
 */
namespace TrackerFieldPayloadPure {

/** Split a "parentId\x1f childId" cascading-select encoding into its two parts. */
void DecodeCascadingSelection(const std::string& encoded, std::string& outParentId, std::string& outChildId);

/** Depth-first lookup of TrackerFieldOption by Id only (used for label resolution). */
const TrackerFieldOption* FindOptionById(const std::vector<TrackerFieldOption>& options, const std::string& id);

/** Depth-first lookup of TrackerFieldOption by Id or display Value (grid often stores labels). */
const TrackerFieldOption* FindOptionByIdOrValue(const std::vector<TrackerFieldOption>& options,
                                                const std::string& idOrValue);

/** Walk options and return the display label for a value (id or value match). Empty if not found. */
std::string ResolveOptionLabel(const std::vector<TrackerFieldOption>& options, const std::string& value);

/**
 * Reduce a structured option's raw JSON payload to the minimal shape Jira accepts
 * on edit/create (id > accountId > groupId+name > key > value > name; passthrough otherwise).
 */
nlohmann::json MinimalPayloadForStructuredOption(const nlohmann::json& raw);

/**
 * Build payload JSON for a TrackerFieldOption (option select / cascading parent).
 * If nestedChildId is non-empty, recurses into option.Children to attach `child`.
 * Returns a disengaged Optional when the option carries no parseable PayloadJson,
 * or the named child is missing.
 */
Optional<nlohmann::json> BuildStructuredOptionPayload(const TrackerFieldOption& option,
                                                      const std::string& nestedChildId);

/**
 * Resolve a field's selected value to a structured option payload using
 * AllowedValueOptions. Handles cascading selects (encoded "parent\x1fchild").
 * Disengaged Optional on a catalog miss.
 */
Optional<nlohmann::json> BuildFieldOptionPayload(const TrackerField& field, const std::string& selectedValue);

/** True iff the string is non-empty and every byte is an ASCII digit. */
bool IsDigitsOnly(const std::string& value);

/**
 * Best-effort scalar-string -> payload for a selectable field when the structured
 * lookup misses (catalog has no PayloadJson, or the value isn't in the catalog).
 * Family / Type / IsUserType drive the shape Jira expects.
 */
nlohmann::json FallbackPayloadForSelectableField(const TrackerField& field, const std::string& scalarValue);

/**
 * Build user-field payload (`{"accountId": "..."}` or null for empty).
 * Handles three input shapes: pre-encoded JSON, displayName lookup against the
 * catalog, or a raw accountId string.
 */
void BuildUserFieldPayload(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue);

/**
 * Parse a numeric string. Empty input yields an engaged JSON null. Non-numeric
 * yields a disengaged Optional. Otherwise the engaged value is the parsed double.
 */
Optional<nlohmann::json> ParseNumberValue(const std::string& rawValue);

/** True when value looks like "PROJ-123" (uppercase / digit project + dash + digits). */
bool LooksLikeIssueKey(const std::string& value);

/** Split CSV input on ',', trim whitespace, drop empties. */
std::vector<std::string> SplitCommaSeparatedTrimmed(const std::string& input);

/** Normalize a Jira label token: trim + replace ASCII whitespace with '-'. */
std::string SanitizeJiraLabelToken(std::string s);

// Public mirrors of TrackerFieldPayload's pure functions (identical behaviour; see that header for
// per-function docs). The production header delegates to these so call sites keep the existing facade.

bool FieldUsesAdfDocument(const TrackerField& field);

std::string ExtractIssueKey(const std::string& value);

bool IsArrayLike(const TrackerField& field);

bool IsSprintField(const TrackerField& field);

std::vector<std::string> SplitCommaSeparatedValues(const std::string& input);

std::string ResolveSprintIdForAgile(const TrackerField& field, const std::string& rawValue);

std::string ResolveDisplayValueForSubmittedSelection(const TrackerField& field, const std::string& value);

// Build the Jira REST `fields` value for a field from raw grid strings. Ok holds the
// built JSON; Err holds the validation message (formerly the outError out-parameter).
Result<nlohmann::json> BuildValue(const TrackerField& field, const std::vector<std::string>& rawValues);

/**
 * Build a Jira ADF comment/worklog body from Markdown-authored text, giving comments the same
 * Markdown→ADF fidelity as the grid long-text editor (BuildAdfScalar / the Plane MarkdownToHtml
 * comment path). Always returns a valid, non-empty ADF `doc`: when the Markdown converts to an
 * empty document (empty / whitespace-only input) it falls back to a single empty paragraph, so
 * Jira never receives an empty-content body it rejects.
 */
nlohmann::json AdfCommentBodyFromMarkdown(const std::string& markdown);

} // namespace TrackerFieldPayloadPure

#endif
