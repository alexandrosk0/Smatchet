#ifndef SMATCHET_CONFIG_SAVE_WORKER_H
#define SMATCHET_CONFIG_SAVE_WORKER_H

#include "Config/ConfigManager.h" // TrackerConfig, AnnotateAnalysisConfig

/// Single-thread coalescing config-save worker.
/// Replaces the per-save detached-thread shims (`ScheduleConfigSaveDetached` /
/// `ScheduleAnnotateConfigSaveDetached`) with one background thread that serializes + coalesces
/// config writes: the latest snapshot **per config-kind** wins (intermediate edits are dropped),
/// and each write goes through the atomic-RMW `ConfigManager::Save` / `SaveAnnotateAnalysis`
/// (serialized by `GetConfigRmwMutexRef`, so it can never lose-update against another writer).
/// Lifecycle: `Start()` once at `AppController` init; `Stop()` (flush-pending-then-join, bounded)
/// before the `ConfigManager` statics tear down at process exit. After `Stop()`, `Enqueue*` falls
/// back to a synchronous save so a write is never lost. No `MainThreadDispatcher` — config saves
/// carry no post-save UI callback (unlike `SmatchetChatPersistWorker`).
namespace smatchet {
namespace config_save {

/// Start the worker. Idempotent — a second `Start()` while running is logged + ignored.
void Start();

/// Flush pending writes within a bounded budget, then join the thread. Idempotent — safe when not
/// running. After return, `Enqueue*` save synchronously on the caller.
void Stop();

/// Enqueue the latest `TrackerConfig` snapshot (coalesced; only the most recent is written when the
/// worker wakes). If the worker isn't running (tests / CLI / pre-init / post-shutdown), saves
/// synchronously on the caller so the write is never lost.
void EnqueueTrackerConfig(const TrackerConfig& cfg);

/// Enqueue the latest `AnnotateAnalysisConfig` snapshot — see `EnqueueTrackerConfig`.
void EnqueueAnnotateConfig(const AnnotateAnalysisConfig& cfg);

/// Enqueue the latest whole-file `PersistentViewsFile` snapshot (`smatchet_views.json`) — the
/// third config-kind slot, added for #2026. The worker runs the same read-modify-write
/// `Views::Save` used to do inline on the UI thread (re-read, fold in out-of-band `ToolbarAppend`
/// writes via `smatchet::views_merge::MergePersistentViewsToolbarAppend`, write); moving that
/// re-read onto the worker is what clears the `LoadPersistentViewsFromDisk` + write violation
/// pair from the frame thread on every pane show/hide. Coalescing is safe: each snapshot is the
/// COMPLETE intended file image, so latest-wins loses nothing. Same not-running fallback as the
/// other kinds.
void EnqueuePersistentViews(const PersistentViewsFile& disk);

// Persisted-field repair hooks (#2047) live in the Config layer, beside the writer they guard:
// see Config/TrackerConfigSaveRepair.h. `EnqueueTrackerConfig` applies them to the snapshot as it
// enters the coalescing slot, because that slot can be filled inside a scenario's pin window and
// drained after the owner has unregistered — by which point the `ConfigManager::Save` chokepoint
// would see no hooks left to apply.

} // namespace config_save
} // namespace smatchet

#endif // SMATCHET_CONFIG_SAVE_WORKER_H
