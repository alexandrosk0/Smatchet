#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "CachedTicketTypes.h"
#include "PendingActionTypes.h"
#include "TicketSyncMeta.h"

/** ISyncCache — the sync/replay-facing surface of the local cache (ADR-0020).
 *
 * The slice of `LocalCacheManager` that `TicketSyncService`, `OfflineQueueService`, and the
 * offline-replay path of `IssueCreatePipeline` consume: cached tickets, the pending-create /
 * pending-field-edit queues, their dead-letter twins, and the `cache_meta` flags. It is NOT
 * the whole local cache — AI chat history and the one-time schema migrations stay concrete-only
 * on `LocalCacheManager` and are deliberately off this interface (see
 * `Source/Core/src/Persistence/CONTEXT.md`).
 *
 * `LocalCacheManager` implements this in production; `tests/support/FakeSyncCache.h` is an
 * in-memory implementation so the service tests run without SQLite. SQLite-free by construction
 * (includes only the rank-0 row headers `CachedTicketTypes.h` / `PendingActionTypes.h`); compiles in both
 * the GL/standalone and DX12 targets.
 */
class ISyncCache {
  public:
    virtual ~ISyncCache() = default;

    // --- Tickets (namespaced by backendKey — multi-grid Slice 1b, ADR-0018) ---
    virtual void SaveTicket(const std::string& backendKey, const CachedTicket& ticket) = 0;
    virtual void SaveTickets(const std::string& backendKey, const std::vector<CachedTicket>& tickets) = 0;
    virtual bool TryGetTicket(const std::string& backendKey, const std::string& ticketId, CachedTicket& out) = 0;
    virtual void DeleteTicket(const std::string& backendKey, const std::string& ticketId) = 0;
    virtual std::vector<CachedTicket> GetAllTickets(const std::string& backendKey) = 0;
    virtual std::vector<std::string> GetAllTicketIds(const std::string& backendKey) = 0;

    // --- Delta-sync metadata (docs/plans/active/delta-sync.md § Phase 2) -------------------
    // The tracker version each cached row was fetched at. Non-pure with conservative defaults, so an
    // implementation that does not store it (a fake, a future backend) simply reports "unknown" and the
    // sync falls back to a full fetch.

    /// Meta of every row under `backendKey` that has one, by ticket id. Default: none (all unknown).
    virtual std::unordered_map<std::string, TicketSyncMeta> GetTicketSyncMeta(const std::string& backendKey) {
        (void)backendKey;
        return {};
    }
    /// SaveTickets plus the meta of each row in one transaction. `metas` is parallel to `tickets`
    /// (a missing or empty-`RemoteUpdated` entry stores no meta and clears any old one). When a row is
    /// saved over a cached row at the SAME `RemoteUpdated`, the two are merged (MergeSameVersionRow) and
    /// the stored field list becomes the union. Default: saves the rows and drops the meta.
    virtual void SaveTicketsWithSyncMeta(const std::string& backendKey, const std::vector<CachedTicket>& tickets,
                                         const std::vector<TicketSyncMeta>& metas) {
        (void)metas;
        SaveTickets(backendKey, tickets);
    }
    /// The rows for `ids` that exist under `backendKey` (missing ids are skipped; order unspecified).
    virtual std::vector<CachedTicket> GetTicketsByIds(const std::string& backendKey,
                                                      const std::vector<std::string>& ids) {
        std::vector<CachedTicket> rows;
        rows.reserve(ids.size());
        for (const std::string& id : ids) {
            CachedTicket row;
            if (TryGetTicket(backendKey, id, row)) {
                rows.push_back(std::move(row));
            }
        }
        return rows;
    }
    /// DeleteTicket for each of `ids` (rows, field values and meta), in one transaction where the
    /// implementation can.
    virtual void DeleteTickets(const std::string& backendKey, const std::vector<std::string>& ids) {
        for (const std::string& id : ids) {
            DeleteTicket(backendKey, id);
        }
    }

    // --- Pending creates ---
    virtual std::int64_t EnqueuePendingCreate(const std::string& backendKey, const std::string& payload) = 0;
    virtual std::vector<PendingCreate> LoadPendingCreates() = 0;
    virtual void UpdatePendingCreate(std::int64_t id, int attempts, const std::string& lastError) = 0;
    virtual void DeletePendingCreate(std::int64_t id) = 0;
    virtual void ArchivePendingCreate(std::int64_t id, const std::string& terminalReason,
                                      const std::string& terminalError) = 0;
    virtual void UpdatePendingCreatePayload(std::int64_t id, const std::string& payload) = 0;
    virtual std::vector<DeadPendingCreate> LoadDeadPendingCreates() = 0;
    virtual std::size_t GetDeadPendingCreateCount() = 0;
    virtual bool RestoreDeadPendingCreate(std::int64_t originalPendingId) = 0;
    virtual void DeleteDeadPendingCreate(std::int64_t deadId) = 0;

    // --- Pending field edits ---
    // NOTE (ADR-0020): the default arguments below are part of the contract and MUST stay
    // identical on every implementation's override — default args on virtuals bind statically
    // by the call-site's static type, so divergent defaults would silently fork the API.
    virtual std::int64_t EnqueuePendingFieldEdit(const std::string& backendKey, const std::string& issueKey,
                                                 const std::string& fieldId, const std::string& fieldsPayloadJson,
                                                 const std::string& originalRichValue = std::string(),
                                                 const std::string& originalValue = std::string(),
                                                 bool hasOriginalValue = false) = 0;
    virtual std::vector<PendingFieldEditRecord> LoadPendingFieldEdits() = 0;
    virtual void UpdatePendingFieldEdit(std::int64_t id, int attempts, const std::string& lastError) = 0;
    virtual void DeletePendingFieldEdit(std::int64_t id) = 0;
    virtual void MarkFieldEditConflict(std::int64_t id, const std::string& contextJson) = 0;
    virtual void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedPayloadJson) = 0;
    virtual void ArchivePendingFieldEdit(std::int64_t id, const std::string& terminalReason,
                                         const std::string& terminalError) = 0;
    virtual std::vector<DeadPendingFieldEdit> LoadDeadPendingFieldEdits() = 0;
    virtual bool RestoreDeadPendingFieldEdit(std::int64_t originalPendingId) = 0;
    virtual void DeleteDeadPendingFieldEdit(std::int64_t deadId) = 0;

    // --- cache_meta flags (idempotent, set-once) ---
    virtual bool HasCacheMetaFlag(const std::string& key) = 0;
    virtual void SetCacheMetaFlag(const std::string& key) = 0;

    // --- Pending actions (comments / worklogs / watch; Quality Pillar 6) ------------------
    // One queue keyed by `kind` (PendingActionTypes.h), FIFO by id, rows namespaced by backend
    // key like the other queues. Failures throw (callers wrap), matching the field-edit methods.
    virtual std::int64_t EnqueuePendingAction(const std::string& backendKey, const std::string& kind,
                                              const std::string& issueKey, const std::string& payloadJson,
                                              const std::string& state) = 0;
    virtual std::vector<PendingActionRecord> LoadPendingActions() = 0;
    virtual void UpdatePendingAction(std::int64_t id, const std::string& state, int attempts,
                                     const std::string& lastError) = 0;
    /// Compare-and-set: updates the row only while its state is still `fromState`. False when the row
    /// is gone (discarded) or has moved to another state, so a caller acting on an older copy of the
    /// row never overrides what happened to it since.
    virtual bool TransitionPendingAction(std::int64_t id, const std::string& fromState, const std::string& toState,
                                         int attempts, const std::string& lastError) = 0;
    virtual void DeletePendingAction(std::int64_t id) = 0;
    /// Move the row to the dead-letter table, keeping its state; `terminalError` (when non-empty)
    /// replaces its last error.
    virtual void ArchivePendingAction(std::int64_t id, const std::string& terminalReason,
                                      const std::string& terminalError) = 0;
    /// Newest first (archived_at DESC, dead_id DESC).
    virtual std::vector<DeadPendingAction> LoadDeadPendingActions() = 0;
    /// Re-queue the newest dead row of `originalId` under its original backend key and state
    /// (attempts reset to 0). False when there is none.
    virtual bool RestoreDeadPendingAction(std::int64_t originalId) = 0;
    virtual void DeleteDeadPendingAction(std::int64_t deadId) = 0;
};
