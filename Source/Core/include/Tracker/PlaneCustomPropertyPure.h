#ifndef SMATCHET_PLANE_CUSTOM_PROPERTY_PURE_H
#define SMATCHET_PLANE_CUSTOM_PROPERTY_PURE_H

// BACKLOG_CODE_REVIEW.md §C4 — pure serializer for Plane custom (work-item property)
// field values. PlaneClient::BuildCreatePayload previously serialized only the built-in
// ids and silently DROPPED every custom (UUID) TrackerField the user filled in. This TU
// emits them under the payload's `properties.<uuid>` object (the shape FetchIssueEditMeta
// documented as the C4 target) so the mutation shells can send them — the server keeps
// the final say on acceptance, surfaced through the normal mutation error path.
// No cpr / Logger / client state — same pure-seam shape as PlaneIssueMappingPure.

#include "SmatchetResult.h"
#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace smatchet {
namespace plane {

// Serialize every custom catalog field present in `fieldValues` into a JSON object keyed
// by property UUID. Grid cells store raw strings, so values are typed per catalog family:
// NUMBER parses to a JSON number, boolean Type to true/false, multi families split on ','
// into an array, and select/user values resolve display labels to option ids when the
// catalog knows them (raw pass-through otherwise). Read-only customs, empty values, and
// ids the catalog doesn't mark IsCustom are skipped. Returns Ok(object; empty when nothing
// applies) or Err(validation message) for a value the family cannot represent.
Result<nlohmann::json> BuildPlaneCustomProperties(const std::unordered_map<std::string, std::string>& fieldValues,
                                                  const std::vector<TrackerField>& catalog);

} // namespace plane
} // namespace smatchet

#endif
