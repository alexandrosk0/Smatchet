#pragma once

// OfflineQueueService — owns the offline-create / offline-field-edit replay queues and the
// dead-letter management around them. Extracted from `AppController_IssueCreateOffline.cpp`
// per CODE_REVIEW.md §1.7 / §7 item 12.
//
// Phase 1A scope (this PR): class skeleton, owned by AppController as a `unique_ptr` member,
// holds the `legacyPendingStartupBanner_` state and a small first batch of read-only
// accessors. AppController's public surface is preserved via thin delegators.
//
// Future phases migrate the remaining methods cluster-by-cluster:
//   1B — write methods (`QueueCreateOffline`, `QueueFieldEditOffline`, `Restore*`, `Delete*`).
//   1C — `Tick*` replay loops + their internal helpers.
//   1D — field-edit equivalents (`ResolveFieldEditConflict`, `Tick…FieldEdits`).
//
// Phase 2 introduces small interface bundles so this service no longer needs `friend` access
// to AppController's private state.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// AppController.h is needed for the nested summary structs that several methods below return
// (`DeadLetterRestoreSummary`, `DeadLetterDeleteSummary`, …). Pulling it here keeps the
// service's method signatures self-documenting at the cost of a heavier include — acceptable
// since this service header is only consumed by AppController-side TUs.
#include "AppController.h"

struct PendingCreate;
struct DeadPendingCreate;
struct PendingFieldEditRecord;
struct DeadPendingFieldEdit;

/// Lifetime contract: AppController owns the service via `std::unique_ptr` and outlives it.
/// The constructor stores an `AppController&` back-reference so methods can reach the
/// still-shared state (`Cache`, `Backend`, audit trail, mutexes). The `friend class
/// OfflineQueueService;` declaration in `AppController.h` is the explicit access seam —
/// no public AppController members were promoted to fit this extraction.
class OfflineQueueService {
  public:
    explicit OfflineQueueService(AppController& app);

    // --- Phase 1A: trivial read-only accessors -------------------------------------------
    std::size_t GetPendingCreateCount() const;
    std::size_t GetDeadPendingCreateCount() const;
    std::vector<PendingCreate> GetPendingCreates() const;
    std::vector<DeadPendingCreate> GetDeadPendingCreates() const;

    /// One-shot getter: returns and clears the legacy-pending-startup banner string. AppController
    /// sets `legacyPendingStartupBanner_` during `Initialize` when it detects legacy queue rows
    /// from a pre-split schema; the UI reads it once on first frame and shows a toast.
    std::string TakeLegacyPendingStartupBanner();

    // --- Phase 1B: write methods + remaining field-edit read accessors -------------------
    /// Persist an offline create row. Returns the new SQLite row id, or 0 on read-only / failure.
    std::int64_t QueueCreateOffline(const IssueDraft& draft);

    /// Move selected dead-letter rows back to the active offline queue (attempts reset to 0).
    AppController::DeadLetterRestoreSummary
    RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds);

    /// Permanently remove dead-letter rows by `pending_creates_dead.dead_id`.
    AppController::DeadLetterDeleteSummary
    DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds);

    /// Permanently remove active offline-queue rows by `pending_creates.id`.
    AppController::PendingQueueDeleteSummary
    DeletePendingCreates(const std::vector<std::int64_t>& pendingIds);

    /// Persist a tracker field payload for later replay when connectivity returns.
    std::int64_t QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                       const std::string& fieldsPayloadJson, std::string& outError,
                                       const std::string& originalRichValue);

    std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const;
    std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const;

    /// Replace the queued payload with a user-resolved version and clear the conflict flag.
    /// The edit will be retried on the next TickOfflineFieldEdits pass.
    void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedMarkdown,
                                  const std::string& richKind);

    AppController::PendingFieldEditDeleteSummary
    DeletePendingFieldEdits(const std::vector<std::int64_t>& ids);

    AppController::DeadFieldEditDeleteSummary
    DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds);

    /// Set by AppController during legacy migration on first launch with a pre-split cache schema.
    /// Reset to empty by `TakeLegacyPendingStartupBanner`. Read/write only on the UI thread.
    std::string legacyPendingStartupBanner_;

  private:
    AppController& app_;
};
