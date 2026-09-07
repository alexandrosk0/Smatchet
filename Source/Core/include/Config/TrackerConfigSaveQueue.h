#ifndef SMATCHET_CONFIG_TRACKER_CONFIG_SAVE_QUEUE_H
#define SMATCHET_CONFIG_TRACKER_CONFIG_SAVE_QUEUE_H

// The coalescing config-save worker's pending-`TrackerConfig` slot, exposed to the Config layer as
// a take-one hook (#2191).
//
// `ConfigManager::Save` is a WHOLE-IMAGE write: it re-reads the file and then rewrites every
// tracker key from the snapshot it was handed. Two writers holding different images therefore
// cannot be ordered by the read-modify-write mutex alone — it makes each write atomic, not
// correctly SEQUENCED. The losing interleaving was not narrow:
//
//   1. the UI enqueues snapshot S1, so the worker wakes and blocks on the RMW mutex
//   2. the UI then runs a synchronous `ConfigManager::Save(S2)` carrying a LATER change
//   3. the worker takes the lock the moment step 2 releases it and writes the stale S1,
//      silently reverting step 2 — visible to the user only on the next launch
//
// The fix is to give the queued snapshot exactly one place it can be written from: INSIDE the RMW
// critical section, immediately before the caller's own image. `ConfigManager::Save` (and
// `ConfigManager::Update`) drain the slot first, so a queued snapshot is always written BEFORE any
// write that was issued after it — never after. The worker no longer removes the snapshot from the
// slot itself; it only asks the Config layer to flush, which is what closes the window where a
// snapshot in a worker local could still land out of order.
//
// The hook indirection keeps the include DAG pointing the right way: the worker layer depends on
// Config, never the reverse (same rationale as `TrackerConfigSaveRepair.h`), so only a forward
// declaration of `TrackerConfig` is needed here.
struct TrackerConfig;

namespace smatchet {
namespace config_save_queue {

/// Move the queued snapshot (if any) into `out` and CLEAR the slot; returns false when the slot is
/// empty. Called with the config read-modify-write lock HELD, so an implementation must not block
/// on anything but its own short slot mutex and must never call back into `ConfigManager`.
using TakePendingFn = bool (*)(TrackerConfig& out);

/// Install (non-null) or clear (null) the take-one hook. The config-save worker installs it while
/// running; with no hook — tests, CLI, pre-init, post-shutdown — `TakePending` reports "nothing
/// queued", which is exactly true because those paths save synchronously.
void SetTakePendingHook(TakePendingFn fn);

/// Take the queued snapshot through the installed hook. False when no hook is installed.
bool TakePending(TrackerConfig& out);

} // namespace config_save_queue
} // namespace smatchet

#endif // SMATCHET_CONFIG_TRACKER_CONFIG_SAVE_QUEUE_H
