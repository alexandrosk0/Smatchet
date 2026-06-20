#pragma once

// Pure, ImGui-free decision helpers extracted from SmatchetProjectPicker::Draw so the
// filtering / key-selection / row-label logic is bucket-A testable and the draw loop stays
// under the function-size branch cap. No ImGui, no global state — referentially transparent.

#include "FieldCatalogCache.h"
#include "TrackerFieldSchema.h"

#include <string>

namespace SmatchetProjectPicker {
namespace detail {

/** Case-insensitive substring test. Empty needle always matches. */
bool ContainsCi(const std::string& haystack, const std::string& needle);

/** Display label for an "All projects" row. Jira / Linear (Team): "KEY — DisplayName"; Plane (or
 *  empty key): display name (or id fallback). */
std::string MakeRowLabel(const RemoteProject& p, const std::string& backendKind);

/** True iff a recently-used cache entry survives the backend/endpoint/search filter and should
 *  render in the "Recently used" section. Empty backendKind/endpoint disable that filter; an entry
 *  with an empty backend/endpoint is treated as a wildcard match. */
bool RecentEntryPasses(const FieldCatalogCache::CachedProjectEntry& e, const std::string& backendKind,
                       const std::string& endpoint, const std::string& filter);

/** The selection key for an "All projects" row: Plane uses the id, everything else uses the key. */
std::string AllProjectKey(const RemoteProject& p, const std::string& backendKind);

/** True iff an "All projects" row (with its computed key) survives the search filter and should
 *  render. A row with an empty key is always rejected. */
bool AllProjectPasses(const std::string& key, const RemoteProject& p, const std::string& filter);

} // namespace detail
} // namespace SmatchetProjectPicker
