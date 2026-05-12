#pragma once

#include "TrackerFieldSchema.h"

#include <string>
#include <vector>

struct TrackerConfig;

/** JSON snapshot under ConfigManager::GetUserDataDirectory() (see .cpp for filename). */
namespace FieldCatalogCache {

/** Stable key for `schema_version` 2 entries: `Jira|<domain>|<project>` or `Plane|<url>|<ws>|<project>`.
 *  `projectKey` is the per-operation project (Jira key, e.g. "PROJ", or Plane project UUID). Confines
 *  the per-project axis to an explicit parameter so PR 4/6 can drop the global `cfg.ProjectKey` without
 *  touching this signature. */
std::string BuildFieldCatalogCacheKey(const TrackerConfig& cfg, const std::string& projectKey);

bool SaveFieldCatalogSnapshot(const std::string& cacheKey, const std::vector<TrackerField>& fields,
                              const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError);

bool TryLoadFieldCatalogSnapshot(const std::string& cacheKey, std::vector<TrackerField>& outFields,
                                 std::vector<TrackerComponent>& outComponents,
                                 std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

} // namespace FieldCatalogCache






