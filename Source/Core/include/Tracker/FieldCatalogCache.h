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
 *  parameter so the global `cfg.ProjectKey` can be dropped without touching this signature. */
std::string BuildFieldCatalogCacheKey(const TrackerConfig& cfg, const std::string& projectKey);

/** Index entry tracking which (backend, endpoint, projectKey) tuples have a cached catalog
 *  on disk, plus an LRU timestamp. The Preferences readout consumes ListCachedProjects(). */
struct CachedProjectEntry {
    std::string projectKey;
    std::string backend; // "Jira", "Plane", or "Linear"
    std::string endpoint; // normalized — Jira domain, Plane API origin (+ workspace slug), or Linear base URL (+ team id).
    std::int64_t lastUsedUnix = 0;
};

/** Save signature: extra (backend, endpoint, projectKey, maxProjects) so the per-disk `entries`
 *  index can be upserted and LRU-capped in one round-trip. `maxProjects` <= 0 means "use default 16". */
bool SaveFieldCatalogSnapshot(const std::string& cacheKey, const std::string& backend, const std::string& endpoint,
                              const std::string& projectKey, int maxProjects,
                              const std::vector<TrackerField>& fields,
                              const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError);

bool TryLoadFieldCatalogSnapshot(const std::string& cacheKey, std::vector<TrackerField>& outFields,
                                 std::vector<TrackerComponent>& outComponents,
                                 std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

/** Sorted by lastUsedUnix descending (most-recent first). Used by the Preferences readout. */
std::vector<CachedProjectEntry> ListCachedProjects();

/** Drops the matching entry (and its blob) from the on-disk cache. Returns true on success
 *  (including "not found" — caller doesn't need to distinguish). Wired to a "Forget" button. */
bool ForgetProject(const std::string& projectKey, const std::string& backend, const std::string& endpoint);

} // namespace FieldCatalogCache
