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
bool TryResolveCascadingSelection(const TrackerField& field, const std::string& currentValue, std::string& outParentId,
                                  std::string& outChildId);

bool IsTimeDurationField(const std::string& fieldId);
bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId);
bool IsNonEditableTimetrackingFieldId(const std::string& fieldId);

// Read-only accessors by design — there is deliberately no Save* counterpart. The shipped write
// path is the Preferences panel mutating `d.cfg` directly + MarkPrefsDirty
// (SmatchetPreferencesUi_Templates.cpp); the Save* helpers deleted here read-modify-wrote the WHOLE
// TrackerConfig, so wiring them back would reintroduce a lost-update race against that path (and
// the sync ConfigManager::Save on the render thread that historical-review finding #732 removed).
// gate-blind-spot-sweep Slice 1b.
std::vector<std::string> LoadDurationSuggestions();
std::vector<std::string> LoadCommentTemplates();

} // namespace TrackerFieldValueUtils






