#pragma once

// Pure, ImGui-free decision helper extracted from DrawJqlQueryEditorEmbedded's project-pill popup
// so the cached-project filter is bucket-A testable and the draw body stays under the
// function-size branch cap. No ImGui, no global state — referentially transparent.

#include "FieldCatalogCache.h"

#include <string>

namespace SmatchetJqlProjectPill {
namespace detail {

/** True iff a cached-project index entry should appear in the project-pill popup for the
 *  given backend and endpoint. Rejects empty keys; an entry whose backend is set and differs
 *  from a non-empty backendKind is filtered out; same for endpoint (empty = wildcard). */
bool EntryPassesPillFilter(const FieldCatalogCache::CachedProjectEntry& e, const std::string& backendKind,
                           const std::string& endpoint);

/** True iff the pill offers recent projects for a tracker of this kind ("Jira", "Plane", "GitHub",
 *  "Linear", as FieldCatalogCache indexes them). A pick rewrites the query as a JQL `project =` clause
 *  or Plane's project_id; GitHub and Linear queries have neither. */
bool PillOffersRecentProjects(const std::string& backendKind);

} // namespace detail
} // namespace SmatchetJqlProjectPill
