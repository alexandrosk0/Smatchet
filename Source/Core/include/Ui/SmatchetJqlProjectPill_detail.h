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

} // namespace detail
} // namespace SmatchetJqlProjectPill
