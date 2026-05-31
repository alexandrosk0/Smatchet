#ifndef SMATCHET_PLANE_FIELD_CATALOG_PURE_H
#define SMATCHET_PLANE_FIELD_CATALOG_PURE_H

// Monoliths Phase A (docs/plans/active/decompose-top-20-monoliths.md) — pure-logic
// JSON → TrackerField / option mapping helpers extracted out of PlaneFieldCatalog.cpp
// (which pulls cpr via PlaneClient_Internal.h) so the doctest rig can exercise the
// branchy Plane-property mapping without HTTP. Mirrors TrackerFieldCatalogPure.h
// (the Jira slice) and PlaneIssueMappingPure.h. Zero cpr / Logger / PlaneClient state.

#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace smatchet {
namespace plane {

/// Map a Plane work-item-property JSON node into a TrackerField. Reproduces the
/// production property_type → {Type, Family, IsArray, IsUserType} mapping
/// byte-for-byte (NUMBER/DATE/DATETIME/OPTION/MULTI_SELECT/MEMBER/MULTI_MEMBER/
/// BOOLEAN and the string+Text default), the OPTION nested-options parse, and the
/// RawFieldDefinitionJson dump. Returns false (leaving `out` untouched) when the
/// node has no usable `id`. Marks IsCustom = true.
bool MapPlanePropertyToField(const nlohmann::json& prop, TrackerField& out);

/// Append a `{id, value}` option to `field`'s AllowedValueOptions from a Plane row
/// (states / cycles / labels share this shape: id required, value falls back from
/// `name` to id). No-op when the row has no usable id. Returns the parsed id (empty
/// when skipped) so the caller can build its parallel cache entry.
std::string AppendPlaneRowOption(const nlohmann::json& row, const char* idKey, const char* nameKey,
                                 TrackerField& field);

} // namespace plane
} // namespace smatchet

#endif // SMATCHET_PLANE_FIELD_CATALOG_PURE_H
