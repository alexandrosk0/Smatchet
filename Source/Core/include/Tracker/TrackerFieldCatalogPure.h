#pragma once

// Pure JSON / vector helpers extracted from TrackerFieldCatalog.cpp.
// Zero ImGui / cpr / SQLite / JiraClient / ITrackerBackend / ConfigManager
// includes — safe to link from the doctest rig under tests/.
// Production callers (`Source/Core/src/TrackerFieldCatalog.cpp`) keep their
// JiraClient.h include for the network-facing catalog-fetch code and route
// component-merge / sort through these free functions.

#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace TrackerFieldCatalogPure {

/// Coerce a Jira component `id` JSON value to a decimal string.
/// Accepts string, integer, unsigned. Anything else → empty string.
std::string ComponentJsonIdToString(const nlohmann::json& value);

/// Resolve a component bean from either a flat `{id,name,...}` object or a
/// wrapped `{componentBean:{id,name,...}}` object. Non-object input returns
/// `nullptr`. Returned pointer aliases `node` and is valid for `node`'s
/// lifetime — do not store across mutations.
const nlohmann::json* ResolveComponentJsonBean(const nlohmann::json& node);

/// Parse a Jira component JSON node into a `TrackerComponent` + matching
/// `TrackerFieldOption`. Returns `true` on success; both outputs are
/// untouched on failure. Required fields: id (string|int|unsigned), name
/// (string). `description` and `self` are copied into the option's
/// PayloadJson when present.
bool ExtractComponentOption(const nlohmann::json& node, TrackerComponent& outComponent, TrackerFieldOption& outOption);

/// Append `component` to `components` (de-dupe by Id; fill blank Name on
/// existing). When `fields` contains a `"components"` field, append the
/// option to its AllowedValueOptions via `MergeTrackerFieldOption`. Safe to
/// call with empty vectors.
void MergeComponentIntoCatalog(std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components,
                               const TrackerComponent& component, const TrackerFieldOption& option);

/// Sort `components` by lower-case Name then Id, and the `"components"`
/// field's AllowedValueOptions the same way. Refreshes the field's
/// AllowedValues + Family afterwards. No-op when the components field is
/// absent.
void SortComponentCatalog(std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components);

/// Build the deduped issuetype `AllowedValues` + `AllowedValueOptions` from the
/// `/rest/api/3/issuetype` JSON array. Dedupe rules:
///  - Drop entries without a usable `id` or `name`.
///  - Dedupe by `id` (later duplicate IDs ignored).
///  - On lower-cased `name` collision, prefer the entry with a
///    `scope.project` block over the un-scoped global template. The global
///    Jira template (e.g. Story id=10008) is rejected at create time when
///    the project has its own scoped variant (e.g. Story id=10004) — see
///    `docs/...` and the BLOOP-103 ship report in the PR that introduced
///    this helper.
/// `outAllowedValues` mirrors the order of `outOptions` after dedup.
void BuildDedupedIssueTypeOptions(const nlohmann::json& issueTypeArray, std::vector<std::string>& outAllowedValues,
                                  std::vector<TrackerFieldOption>& outOptions);

/// Parse one `/rest/api/3/field` array element into a `TrackerField`. Returns
/// `false` (leaving `outField` untouched) when the node lacks string `id` /
/// `name`. On success fills Id/Name/Type/ReadOnly/schema-derived flags
/// (IsArray, ItemsType, IsUserType, SchemaSystem, SchemaCustom), IsCustom,
/// Family, and RawFieldDefinitionJson. When the field's `schema.custom`
/// contains `gh-sprint`, its id is appended to `outSprintFieldIds`.
bool ParseFieldDefinition(const nlohmann::json& field, TrackerField& outField,
                          std::vector<std::string>& outSprintFieldIds);

/// Rebuild `field`'s AllowedValues + AllowedValueOptions from a flat Jira
/// catalog array (`/priority`, `/status`, …) of `{id,name,…}` objects.
/// Dedupes by coerced id, skips entries with empty id/name, stores each
/// object's `dump()` as the option PayloadJson, and refreshes Family.
/// `array` that is not a JSON array clears nothing and returns immediately.
void BuildSimpleCatalogOptions(const nlohmann::json& array, TrackerField& field);

/// Apply a `/rest/api/3/issue/createmeta` response to the catalog: collect
/// unique issue-type names, upsert `TrackerIssueTypeCreateMeta` rows (merging
/// required-field ids), merge per-field `allowedValues` into matching catalog
/// fields, and harvest `components` allowedValues into `outComponents`.
/// Mirrors the in-place createmeta enrichment loop. `fieldIndexById` maps
/// field id → index into `fields`. No-op when `metaJson` has no `projects`
/// array.
void ApplyCreateMetaToCatalog(const nlohmann::json& metaJson, const std::string& projectKey,
                              const std::unordered_map<std::string, std::size_t>& fieldIndexById,
                              std::vector<TrackerField>& fields, std::vector<TrackerComponent>& components,
                              std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta,
                              std::set<std::string>& outUniqueIssueTypes);

/// Build sorted issue-type options from a `/rest/api/3/project/{key}` JSON
/// object's `issueTypes` array. Returns `true` and fills `outOptions`
/// (sorted by Value) when at least one usable `{id,name}` is found; returns
/// `false` with `outOptions` cleared otherwise.
bool BuildIssueTypeOptionsFromProjectJson(const nlohmann::json& projectJson,
                                          std::vector<TrackerFieldOption>& outOptions);

/// Extract de-duped positive board ids from a `/rest/agile/1.0/board` page
/// JSON object's `values` array, appending into `outBoardIds` (skipping ids
/// already in `seenBoardIds`). Returns the page's `isLast` flag (true also
/// when `values` is empty or the response shape is unusable, signalling the
/// caller to stop paging).
bool ExtractSprintBoardIdsFromPage(const nlohmann::json& boardsJson, std::set<int>& seenBoardIds,
                                   std::vector<int>& outBoardIds);

/// Append sprint options from a `/rest/agile/1.0/board/{id}/sprint` JSON
/// object's `values` array into `outOptions`, de-duping by coerced sprint id
/// via `seenSprintIds`. Each option stores the sprint object `dump()` as
/// PayloadJson. No-op when the response shape is unusable.
void CollectSprintOptionsFromPage(const nlohmann::json& sprintJson, std::set<std::string>& seenSprintIds,
                                  std::vector<TrackerFieldOption>& outOptions);

} // namespace TrackerFieldCatalogPure
