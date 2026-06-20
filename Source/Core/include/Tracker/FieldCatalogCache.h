#pragma once

#include "TrackerFieldSchema.h"

#include <cstdint>
#include <string>
#include <vector>

struct TrackerConfig;

/** JSON snapshot under ConfigManager::GetUserDataDirectory() (see .cpp for filename). */
namespace FieldCatalogCache {

/** Stable key for `schema_version` 3 entries: `Jira|<domain>|<project>`, `Plane|<url>|<ws>|<project>`,
 *  or `Linear|<baseUrl>|<teamId>|<project>`. `projectKey` is the per-operation project (Jira key, e.g.
 *  "PROJ", Plane project UUID, or Linear team key). Confines the per-project axis to an explicit
 *  parameter so PR 4/6 can drop the global `cfg.ProjectKey` without touching this signature. */
std::string BuildFieldCatalogCacheKey(const TrackerConfig& cfg, const std::string& projectKey);

/** PR 3: index entry tracking which (backend, endpoint, projectKey) tuples have a cached catalog
 *  on disk, plus an LRU timestamp. PR 6's Preferences readout consumes ListCachedProjects(). */
struct CachedProjectEntry {
    std::string projectKey;
    std::string backend; // "Jira", "Plane", or "Linear"
    std::string endpoint; // normalized — Jira domain, Plane API origin (+ workspace slug), or Linear base URL (+ team id).
    std::int64_t lastUsedUnix = 0;
};

/** PR 3 save signature: extra (backend, endpoint, projectKey, maxProjects) so the per-disk `entries`
 *  index can be upserted and LRU-capped in one round-trip. `maxProjects` <= 0 means "use default 16". */
bool SaveFieldCatalogSnapshot(const std::string& cacheKey, const std::string& backend, const std::string& endpoint,
                              const std::string& projectKey, int maxProjects,
                              const std::vector<TrackerField>& fields,
                              const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError);

bool TryLoadFieldCatalogSnapshot(const std::string& cacheKey, std::vector<TrackerField>& outFields,
                                 std::vector<TrackerComponent>& outComponents,
                                 std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

/** PR 3: sorted by lastUsedUnix descending (most-recent first). Used by PR 6's Preferences readout. */
std::vector<CachedProjectEntry> ListCachedProjects();

/** PR 3: drops the matching entry (and its blob) from the on-disk cache. Returns true on success
 *  (including "not found" — caller doesn't need to distinguish). PR 6 wires this to a "Forget" button. */
bool ForgetProject(const std::string& projectKey, const std::string& backend, const std::string& endpoint);

} // namespace FieldCatalogCache
