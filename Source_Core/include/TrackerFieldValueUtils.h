#pragma once

#include "TrackerFieldSchema.h"

#include <string>
#include <vector>

namespace TrackerFieldValueUtils {

const TrackerFieldOption* FindOptionRecursive(const std::vector<TrackerFieldOption>& options, const std::string& value);
std::string ResolveOptionId(const TrackerField& field, const std::string& value);
std::string ResolveOptionLabel(const TrackerField& field, const std::string& value);
std::vector<std::string> ResolveCurrentSelectionIds(const TrackerField& field, const std::string& currentValue);
const char* EmptySelectPreviewLabel(const TrackerField& field);
std::string BuildSelectionPreview(const TrackerField& field, const std::vector<std::string>& selectedIds);
std::string BuildCascadingPreview(const TrackerFieldOption& parent, const TrackerFieldOption* child);
bool TryResolveCascadingSelection(const TrackerField& field, const std::string& currentValue, std::string& outParentId,
                                  std::string& outChildId);

std::vector<std::string> LoadDurationSuggestions();
void SaveDurationSuggestions(const std::vector<std::string>& list);

std::vector<std::string> LoadCommentTemplates();
void SaveCommentTemplates(const std::vector<std::string>& list);

} // namespace TrackerFieldValueUtils






