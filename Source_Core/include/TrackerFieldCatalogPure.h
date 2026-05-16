#pragma once

// Pure JSON / vector helpers extracted from TrackerFieldCatalog.cpp.
// Zero ImGui / cpr / SQLite / JiraClient / ITrackerClient / ConfigManager
// includes — safe to link from the doctest rig under tests/.
//
// Production callers (`Source_Core/src/TrackerFieldCatalog.cpp`) keep their
// JiraClient.h include for the network-facing catalog-fetch code and route
// component-merge / sort through these free functions.

#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
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
bool ExtractComponentOption(const nlohmann::json& node,
                            TrackerComponent& outComponent,
                            TrackerFieldOption& outOption);

/// Append `component` to `components` (de-dupe by Id; fill blank Name on
/// existing). When `fields` contains a `"components"` field, append the
/// option to its AllowedValueOptions via `MergeTrackerFieldOption`. Safe to
/// call with empty vectors.
void MergeComponentIntoCatalog(std::vector<TrackerField>& fields,
                               std::vector<TrackerComponent>& components,
                               const TrackerComponent& component,
                               const TrackerFieldOption& option);

/// Sort `components` by lower-case Name then Id, and the `"components"`
/// field's AllowedValueOptions the same way. Refreshes the field's
/// AllowedValues + Family afterwards. No-op when the components field is
/// absent.
void SortComponentCatalog(std::vector<TrackerField>& fields,
                          std::vector<TrackerComponent>& components);

} // namespace TrackerFieldCatalogPure
