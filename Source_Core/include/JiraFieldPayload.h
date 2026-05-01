#ifndef SMATCHET_JIRA_FIELD_PAYLOAD_H
#define SMATCHET_JIRA_FIELD_PAYLOAD_H

#include "JiraClient.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/**
 * Shared Jira field -> JSON payload builder. Extracted from
 * AppController::SubmitFieldEditNetworkOnly so it can be reused by the
 * create pipeline (single draft + bulk import + offline replay).
 *
 * Callers are responsible for:
 *   - Sprint fields: never put them in REST v3 `fields` for create (Jira expects
 *     numeric sprint ids on Agile only). Use `IsSprintField` to skip, then
 *     `ResolveSprintIdForAgile` + `ITrackerClient::AddIssueToSprint` after create.
 *   - Timetracking containers (merge originalEstimate/remainingEstimate).
 */
namespace JiraFieldPayload {

/**
 * True when Jira Cloud REST v3 expects an Atlassian Document (ADF) object for this field
 * (system description / environment, textarea-style customfields, wiki renderer, etc.).
 */
bool FieldUsesAdfDocument(const JiraField& field);

/**
 * Convert raw string values into the JSON value for Jira's `fields` map.
 * Returns false + sets outError on invalid date/number input. Empty scalar
 * values become `null` (meaning "clear field") unless the field is an array
 * type, where an empty list is produced.
 */
bool BuildValue(const JiraField& field, const std::vector<std::string>& rawValues, nlohmann::json& outValue,
                std::string& outError);

/**
 * Resolve a Jira issue key ("PROJ-123") from either a raw key or "PROJ-123 - Summary..."
 * Returns an empty string if no key can be extracted.
 */
std::string ExtractIssueKey(const std::string& value);

/** True for array/multi allow-list fields (options, components, users, labels, sprints, ...). */
bool IsArrayLike(const JiraField& field);

/** Agile sprint field: must not be embedded in issue create `fields` JSON. */
bool IsSprintField(const JiraField& field);

/** Comma-separated tokens (trim each, drop empties). */
std::vector<std::string> SplitCommaSeparatedValues(const std::string& input);

/**
 * Map draft/grid text (option id, label, or plain digits) to numeric sprint id for Agile API.
 * Returns empty when the value cannot be resolved.
 */
std::string ResolveSprintIdForAgile(const JiraField& field, const std::string& rawValue);

/**
 * Value to store in CachedTicket.fieldValues after a successful edit (option labels, cascading "a > b").
 */
std::string ResolveDisplayValueForSubmittedSelection(const JiraField& field, const std::string& value);

} // namespace JiraFieldPayload

#endif
