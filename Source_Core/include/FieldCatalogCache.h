#pragma once

#include "TrackerFieldSchema.h"

#include <string>
#include <vector>

/** JSON snapshot under ConfigManager::GetFilesBaseDirectory() (see .cpp for filename). */
namespace FieldCatalogCache {

bool SaveFieldCatalogSnapshot(const std::vector<TrackerField>& fields, const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError);

bool TryLoadFieldCatalogSnapshot(std::vector<TrackerField>& outFields, std::vector<TrackerComponent>& outComponents,
                                 std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);

} // namespace FieldCatalogCache
