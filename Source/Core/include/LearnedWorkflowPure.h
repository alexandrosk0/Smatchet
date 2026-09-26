#pragma once

#include "Tracker/TrackerFieldSchema.h"

#include <string>
#include <vector>

namespace smatchet::workflow {

extern const char* kLearnedTransitionsKind;

std::string BuildLearnedTransitionsKey(const std::string& projectKey, const std::string& issueTypeKey,
                                       const std::string& fromStatusKey);

std::string SerializeTransitionTargets(const std::vector<TrackerFieldOption>& options);

bool ParseTransitionTargets(const std::string& json, std::vector<TrackerFieldOption>& out);

} // namespace smatchet::workflow
