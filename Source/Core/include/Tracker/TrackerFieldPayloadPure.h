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

/// `rawValues` without its empty entries (an empty entry means "no value", never a value to send).
std::vector<std::string> NonEmptyValues(const std::vector<std::string>& rawValues);

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

// Payloads for the two field edits that are not a plain `fields` entry: sprint membership (sent with
// Jira's agile API) and the time-tracking estimates (the compound `timetracking` field). The live edit,
// the offline queue, replay and conflict resolution all build and read them here (Quality Pillar 6).

/// Key of the offline-queue payload for a sprint edit, {"sprint_add": "<sprint id>"}. Replay sends it with
/// AddIssueToSprint, never as a `fields` update.
constexpr const char* kSprintAddPayloadKey = "sprint_add";

nlohmann::json MakeSprintAddPayload(const std::string& sprintId);

/// True, with `outSprintId` set, when `payload` is exactly {"sprint_add": "<non-empty string>"}.
bool TryParseSprintAddPayload(const nlohmann::json& payload, std::string& outSprintId);

/// The `timetracking` key an editable estimate field maps to ("originalEstimate" for
/// `timeoriginalestimate`, "remainingEstimate" for `timeestimate`), or nullptr for any other field.
const char* TimetrackingKeyForEstimateField(const std::string& fieldId);

struct TimetrackingEstimateEdit {
    nlohmann::json FieldsPayload;  ///< {"timetracking": {"originalEstimate", "remainingEstimate"}}
    std::string OriginalEstimate;  ///< value sent for `timeoriginalestimate` (the display to apply)
    std::string RemainingEstimate; ///< value sent for `timeestimate`
};

/// The `fields` payload for an edit of one estimate: the edited estimate takes `editedValue` and the other
/// keeps its current value, since Jira recalculates an estimate the request leaves out. Err when the value
/// is empty (clearing is not supported) or `fieldId` is not an editable estimate.
Result<TimetrackingEstimateEdit> BuildTimetrackingEstimateEdit(const std::string& fieldId,
                                                               const std::string& editedValue,
                                                               const std::string& originalEstimate,
                                                               const std::string& remainingEstimate);

/// The estimate field an estimate edit does NOT change ("timeestimate" for `timeoriginalestimate` and
/// vice versa), or nullptr when `fieldId` is not an editable estimate.
const char* OtherEstimateFieldId(const std::string& fieldId);

/// For a queued estimate payload ({"timetracking": {...}} for `editedFieldId`): set the estimate the user
/// did not edit to `currentOtherEstimate`, the tracker's value now, so a replay never writes a stale copy
/// back over a change made meanwhile. An empty current value drops the key (Jira keeps or recalculates an
/// estimate the request leaves out). The edited estimate is never touched. False, `payload` unchanged,
/// when it is not an estimate payload.
bool RefreshUntouchedEstimate(const std::string& editedFieldId, const std::string& currentOtherEstimate,
                              nlohmann::json& payload);

/// The user's queued value for `fieldId` as text for the offline-conflict dialog: the sprint id of a
/// sprint payload, the edited estimate of a `timetracking` payload, else the value under `fieldId` (or
/// `fieldId`_html) — a string as-is, anything else as JSON only when `stringifyStructured`. False when
/// the payload holds no such value.
bool TryDescribeQueuedFieldValue(const std::string& fieldId, const nlohmann::json& payload, bool stringifyStructured,
                                 std::string& out);

enum class SpecialPayloadRebuild : unsigned char {
    NotSpecial,   ///< not a sprint / estimate payload: the caller builds it the usual way
    Rebuilt,      ///< `out` holds the payload for `resolvedValue`
    Unresolvable, ///< a sprint / estimate payload, but `resolvedValue` cannot fill it (unknown sprint, empty)
};

/// Rebuild a queued sprint / estimate payload around the value the user chose in the conflict dialog. A
/// sprint takes a sprint name or id (resolved against `field`, when known); an estimate replaces only the
/// edited estimate and keeps the other one as queued.
SpecialPayloadRebuild RebuildSpecialQueuedPayload(const std::string& fieldId, const nlohmann::json& queued,
                                                  const TrackerField* field, const std::string& resolvedValue,
                                                  nlohmann::json& out);

} // namespace TrackerFieldPayloadPure

#endif
