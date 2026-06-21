#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CachedTicketTypes.h"

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
 * (includes only `CachedTicketTypes.h`); compiles in both the GL/standalone and DX12 targets.
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
};
