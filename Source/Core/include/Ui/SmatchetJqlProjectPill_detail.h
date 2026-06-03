#pragma once

// Pure, ImGui-free decision helper extracted from DrawJqlQueryEditorEmbedded's project-pill popup
// so the cached-project filter is bucket-A testable and the draw body stays under the
// function-size branch cap. No ImGui, no global state — referentially transparent.

#include "FieldCatalogCache.h"

#include <string>

namespace SmatchetJqlProjectPill {
namespace detail {

/** True iff a cached-project index entry should appear in the Jira project-pill popup for the
 *  given tracker domain. Rejects empty keys and non-Jira backends; an entry whose endpoint is set
 *  and differs from a non-empty domain is filtered out (empty endpoint / empty domain = wildcard). */
bool EntryPassesPillFilter(const FieldCatalogCache::CachedProjectEntry& e, const std::string& domain);

} // namespace detail
} // namespace SmatchetJqlProjectPill
