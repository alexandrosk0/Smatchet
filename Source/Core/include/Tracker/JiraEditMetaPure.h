#pragma once

// Pure editmeta-field decision logic extracted from JiraUserAndMeta.cpp
// (FetchIssueEditMeta, which pulls cpr). Zero ImGui / cpr / SQLite / JiraClient /
// ITrackerBackend / ConfigManager includes — safe to link from the doctest rig
// under tests/. Mirrors the TrackerFieldCatalogPure / JiraIssueMappingPure splits.

#include <nlohmann/json.hpp>

#include <string>

namespace smatchet {
namespace jira {

/// Decide whether a single Jira editmeta field entry is editable.
/// `fieldId` is the lower-cased field key; `meta` is the per-field editmeta object.
/// Reproduces the original inline rule byte-for-byte:
///   - editable when an `operations` array lists "set" / "add" / "remove" (string entries
///     directly, or object entries via the "operation" / "name" / "type" keys);
///   - else, when `operations` is absent or not an array, editable for nullable date/datetime
///     schema fields, except the schema-only created/updated/resolutiondate denylist.
bool JiraEditMetaFieldCanEdit(const std::string& fieldId, const nlohmann::json& meta);

} // namespace jira
} // namespace smatchet
