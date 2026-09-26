#pragma once

// LearnedWorkflowPure — the remembered ("learned") workflow edges behind the offline status combo
// (Quality Pillar 6). A successful live transitions fetch is stored per (project, issue type, from
// status) in the lookup cache, so the combo can offer valid moves while the tracker is unreachable.
// Pure: no I/O, no Logger.h.

#include "Tracker/TrackerFieldSchema.h"

#include <string>
#include <vector>

namespace smatchet {
namespace workflow {

/// `lookup_cache.kind` of the learned transition rows.
constexpr const char* kLearnedTransitionsKind = "workflow_transitions";

/// `projectKey|issueTypeKey|fromStatusKey`, or "" when any part is empty (nothing to learn under).
std::string BuildLearnedTransitionsKey(const std::string& projectKey, const std::string& issueTypeKey,
                                       const std::string& fromStatusKey);

/// A JSON array of `{"id", "name"}` objects, one per target status.
std::string SerializeTransitionTargets(const std::vector<TrackerFieldOption>& options);

/// Inverse of SerializeTransitionTargets. False on a parse error or a non-array; entries with neither
/// an `id` nor a `name` are skipped.
bool ParseTransitionTargets(const std::string& json, std::vector<TrackerFieldOption>& out);

} // namespace workflow
} // namespace smatchet
