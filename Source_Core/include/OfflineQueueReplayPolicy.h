#pragma once

// Cap-decision logic for the offline queue replay loop. Lives in its own header
// with zero banned includes (no SQLite, no cpr, no ImGui) so the doctest rig
// under tests/ can exercise the boundary without dragging the full
// LocalCacheManager.h / SQLite cascade. See backlog/AGENT_SELF_IMPROVEMENT.md
// entry "2026-05-15 · test-rig · OfflineCreateQueue::kMaxReplayAttempts".
//
// Source of truth for the replay-attempt cap. LocalCacheManager.h's
// OfflineCreateQueue::kMaxReplayAttempts / OfflineFieldEditQueue::kMaxReplayAttempts
// are kept as aliases to this constant so existing call sites continue to
// compile without churn.

namespace OfflineQueueReplayPolicy {

/// Maximum replay attempts per offline-queued entry before it is moved to the
/// dead-letter (archive) bucket. Applies to both pending_creates and
/// pending_field_edits.
constexpr int kMaxReplayAttempts = 5;

/// Decision boundary for the offline-replay loop: returns true when an entry
/// that has reached `currentAttempts` attempts has exhausted its budget and
/// must be archived instead of retried.
///
/// Called in two shapes inside OfflineQueueService::TickOfflineFieldEdits /
/// TickOfflineCreates:
///   * Pre-attempt gate — `ShouldArchive(row.Attempts)` short-circuits replay
///     for rows that already reached the cap from a previous tick.
///   * Post-failure gate — `ShouldArchive(row.Attempts + 1)` archives a row
///     whose just-attempted replay tipped it over the cap.
inline bool ShouldArchive(int currentAttempts, int maxAttempts = kMaxReplayAttempts) {
    return currentAttempts >= maxAttempts;
}

} // namespace OfflineQueueReplayPolicy
