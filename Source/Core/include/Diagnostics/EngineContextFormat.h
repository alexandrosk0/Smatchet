#ifndef SMATCHET_DIAGNOSTICS_ENGINE_CONTEXT_FORMAT_H
#define SMATCHET_DIAGNOSTICS_ENGINE_CONTEXT_FORMAT_H

#include <string>

#include <nlohmann/json.hpp>

// EngineContextFormat — pure formatter turning the host-engine context snapshot
// (EngineHostContext.h JSON, pushed by the Unreal plugin) into the markdown block
// prefilled into the quick-create issue description. Pure function of
// (snapshot, toggles) so the doctest rig covers it without a host or UI.
//
// docs/plans/quick-create-issue-unreal-context.md.

struct TrackerConfig;

namespace smatchet {
namespace hostctx {

/// Per-item include switches mirroring the Preferences checkboxes. A field is
/// emitted only when its toggle is on AND the snapshot carries it — missing
/// keys are skipped silently so packaged-game snapshots (no editor data) and
/// older hosts degrade cleanly.
struct EngineContextToggles {
    bool EngineVersion = true;
    bool Project = true;
    bool Platform = true;
    bool Level = true;
    bool PieState = true;
    bool SelectedActors = true;
    bool LogTail = true;
    int LogTailLines = 50; // last-N Output Log lines to embed (clamped to [1, 300])
};

/// Read the toggle set from the persisted preferences.
EngineContextToggles TogglesFromConfig(const TrackerConfig& cfg);

/// Format `snapshot` into a markdown context block, or "" when the snapshot is
/// not a JSON object or no enabled field is present. The actor list is capped
/// at 25 entries (an "…and N more" line follows); the log tail renders as a
/// fenced code block truncated to the last `LogTailLines` lines.
std::string BuildEngineContextMarkdown(const nlohmann::json& snapshot, const EngineContextToggles& toggles);

} // namespace hostctx
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_ENGINE_CONTEXT_FORMAT_H
