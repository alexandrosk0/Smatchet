#ifndef SMATCHET_CONFIG_TRACKER_CONFIG_SAVE_REPAIR_H
#define SMATCHET_CONFIG_TRACKER_CONFIG_SAVE_REPAIR_H

#include <functional>

// Persisted-`TrackerConfig` repair hooks, applied on the way to disk (#2047).
//
// A screenshot-capture scenario temporarily overwrites fields of the live config that are
// PERSISTED, not session-scoped: `GitHubPat` above all, plus the git-repo legs, `UiMode`,
// `VcsFeedLayout`, `WhisperSetupCompleted` and `UpdateCheckEnabled`. It cannot unwind them inline
// when it finishes (that would un-pin the state before the frame being captured is drawn), so the
// unwind is deferred by a frame. Any config save issued inside that window — and there are dozens
// of direct `ConfigManager::Save(g_ui.cfg)` call sites across the UI, plus a process that simply
// exits with the unwind still queued — wrote the scenario's CLEARED GitHub PAT over the user's real
// credential. A hook makes that write UNISSUABLE rather than unlikely: `ConfigManager::Save`
// applies every registered hook to a copy of the outgoing config before serializing it, so the
// pinned fields reach disk with the USER's values while every other edit in the same snapshot is
// written normally. No save path can miss the repair, because `ConfigManager::Save` is the single
// seam every `TrackerConfig` write funnels through, and no legitimate save is dropped.
//
// This lives in the Config layer, beside the writer it guards: the hazard is a property of
// persistence, not of the coalescing worker (`ConfigSaveWorker`) that is merely one of the writers.
// Keeping it here also points the include DAG the right way — the Commands/Scenarios and worker
// layers depend on Config, never the reverse. Only a forward declaration of `TrackerConfig` is
// needed, so this header adds no include edges at all.
struct TrackerConfig;

namespace smatchet {
namespace config_repair {

/// Register a repair for persisted `TrackerConfig` fields that some transient in-process actor has
/// temporarily overwritten on the live config. Returns a token for `UnregisterTrackerConfigRepair`,
/// or 0 for an empty `fn` (which registers nothing).
/// Hooks must put back ONLY the fields their owner pinned — a hook that rewrites anything else
/// would silently discard the user's legitimate edits to those fields. They run on the saving
/// thread (any thread), with the config read-modify-write lock HELD, so a hook must be a plain
/// field assignment: it must not block, and it must not call back into `ConfigManager` (`Save` /
/// `Load` / `InvalidateCache`) or it will deadlock. Registration itself is thread-safe.
int RegisterTrackerConfigRepair(std::function<void(TrackerConfig&)> fn);

/// Drop a hook registered by `RegisterTrackerConfigRepair`. Idempotent — an unknown, already-dropped
/// or 0 token is a no-op, so an owner can unregister unconditionally from its unwind path.
void UnregisterTrackerConfigRepair(int token);

/// Apply every registered repair to `cfg`, in registration order.
/// Called by `ConfigManager::Save` (the chokepoint that makes the bad write unissuable) and by
/// `config_save::EnqueueTrackerConfig`. The worker's call is NOT redundant: the coalescing slot can
/// be filled inside the pin window and drained after the owner has unregistered, at which point the
/// `Save` chokepoint would see no hooks and write the pinned value. Repairing the snapshot as it
/// enters the queue closes that gap. Applying twice is harmless — a hook is idempotent by contract.
void ApplyTrackerConfigRepairs(TrackerConfig& cfg);

} // namespace config_repair
} // namespace smatchet

#endif // SMATCHET_CONFIG_TRACKER_CONFIG_SAVE_REPAIR_H
