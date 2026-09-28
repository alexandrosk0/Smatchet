#pragma once

// FieldOptionsJsonPure — the one JSON codec for TrackerFieldOption, shared by the on-disk field-catalog
// snapshot (FieldCatalogCache) and the offline lookup rows (Quality Pillar 6: per-project component
// options). Full fidelity: id, value, secondary text, the raw option payload (edit payloads are built
// from it) and nested children. Pure: no I/O, no Logger.h.

#include "Tracker/TrackerFieldSchema.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace fieldoptions {

/// Children nested deeper than this are dropped (a cascading select has two levels).
constexpr int kMaxOptionDepth = 16;

nlohmann::json FieldOptionToJson(const TrackerFieldOption& option);

/// Inverse of FieldOptionToJson. False when `j` is not an object. Throws nlohmann::json::type_error
/// when a member has the wrong type; ParseFieldOptions catches it.
bool FieldOptionFromJson(const nlohmann::json& j, TrackerFieldOption& out);

/// A JSON array of FieldOptionToJson objects. Invalid UTF-8 in tracker text is replaced, never thrown.
std::string SerializeFieldOptions(const std::vector<TrackerFieldOption>& options);

/// Inverse of SerializeFieldOptions. False on a parse error, a non-array or a mistyped member; never
/// throws. Non-object entries are skipped.
bool ParseFieldOptions(const std::string& json, std::vector<TrackerFieldOption>& out);

/// The compact form: a JSON array of {"id", "name"} (Value as "name"), for rows that need no more.
std::string SerializeOptionIdNames(const std::vector<TrackerFieldOption>& options);

/// Inverse of SerializeOptionIdNames. False on a parse error or a non-array; never throws. Entries with
/// neither an id (string or integer) nor a string name are skipped.
bool ParseOptionIdNames(const std::string& json, std::vector<TrackerFieldOption>& out);

} // namespace fieldoptions
} // namespace smatchet
