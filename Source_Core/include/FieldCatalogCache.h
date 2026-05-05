#pragma once

#include "TrackerFieldSchema.h"

#include <string>
#include <vector>

struct JiraConfig;

/** JSON snapshot under ConfigManager::GetFilesBaseDirectory() (see .cpp for filename). */
namespace FieldCatalogCache {

/** Stable key for `schema_version` 2 entries: `Jira|<domain>|<project>` or `Plane|<url>|<ws>|<project>`. */
std::string BuildFieldCatalogCacheKey(const JiraConfig& cfg);

bool SaveFieldCatalogSnapshot(const std::string& cacheKey, const std::vector<TrackerField>& fields,
                              const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError);

bool TryLoadFieldCatalogSnapshot(const std::string& cacheKey, std::vector<TrackerField>& outFields,
                                 std::vector<TrackerComponent>& outComponents,
                                 std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

} // namespace FieldCatalogCache
