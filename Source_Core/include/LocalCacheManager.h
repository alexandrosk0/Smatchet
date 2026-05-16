#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "CachedTicketTypes.h"
#include "OfflineQueueReplayPolicy.h"

/** Max replay attempts for offline `pending_creates` before archive or drop.
 *  Aliases `OfflineQueueReplayPolicy::kMaxReplayAttempts` so the SQLite-free
 *  policy header stays the single source of truth (tests target the policy
 *  header without pulling LocalCacheManager + SQLite). */
namespace OfflineCreateQueue {
constexpr int kMaxReplayAttempts = OfflineQueueReplayPolicy::kMaxReplayAttempts;
}

/** Max replay attempts for offline `pending_field_edits` before dead-letter archive.
 *  Aliases `OfflineQueueReplayPolicy::kMaxReplayAttempts` — same rationale. */
namespace OfflineFieldEditQueue {
constexpr int kMaxReplayAttempts = OfflineQueueReplayPolicy::kMaxReplayAttempts;
}

class LocalCacheManager {
  public:
    explicit LocalCacheManager(const std::string& dbPath);

    void SaveTicket(const CachedTicket& ticket);
    /** @return false if `ticketId` is not present in `tickets`. */
    bool TryGetTicket(const std::string& ticketId, CachedTicket& out);
    void DeleteTicket(const std::string& ticketId);
    std::vector<CachedTicket> GetAllTickets();
    /** Streaming overload — calls `fn` once per fully-populated ticket instead of materialising the
     *  entire result set. Avoids the O(N) in-memory join for bulk-sync paths (§4.3). */
    void ForEachTicket(const std::function<void(CachedTicket&&)>& fn);
    std::vector<std::string> GetAllTicketIds();

    /** @return generated row id. */
    std::int64_t EnqueuePendingCreate(const std::string& payload);
    std::vector<PendingCreate> LoadPendingCreates();
    void UpdatePendingCreate(std::int64_t id, int attempts, const std::string& lastError);
    void DeletePendingCreate(std::int64_t id);
    void ArchivePendingCreate(std::int64_t id, const std::string& terminalReason, const std::string& terminalError);
    /** PR 5 legacy-project sweep: replace the stored `payload` JSON for an active pending row. */
    void UpdatePendingCreatePayload(std::int64_t id, const std::string& payload);

    /** Generic `cache_meta` flag helpers used by one-shot migration sweeps. */
    bool HasCacheMetaFlag(const std::string& key);
    void SetCacheMetaFlag(const std::string& key);
    std::vector<DeadPendingCreate> LoadDeadPendingCreates();
    size_t GetDeadPendingCreateCount();

    /**
     * One-time: delete legacy `pending_creates` rows already at retry cap (pre dead-letter).
     * Persists a meta flag per database file.
     */
    size_t RunOneTimeLegacyDropPendingAtMaxAttempts();

    /** Restore latest dead-letter row for this original pending id back to active queue (attempts=0). */
    bool RestoreDeadPendingCreate(std::int64_t originalPendingId);

    /** Permanently remove a dead-letter row (user discard). */
    void DeleteDeadPendingCreate(std::int64_t deadId);

    std::int64_t EnqueuePendingFieldEdit(const std::string& issueKey, const std::string& fieldId,
                                         const std::string& fieldsPayloadJson,
                                         const std::string& originalRichValue = std::string());
    std::vector<PendingFieldEditRecord> LoadPendingFieldEdits();
    void UpdatePendingFieldEdit(std::int64_t id, int attempts, const std::string& lastError);
    void DeletePendingFieldEdit(std::int64_t id);
    /// Mark a pending field edit as having a 3-way merge conflict; stores the context JSON for the UI.
    void MarkFieldEditConflict(std::int64_t id, const std::string& contextJson);
    /// Clear the conflict flag and replace the payload with the user-resolved version.
    void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedPayloadJson);
    void ArchivePendingFieldEdit(std::int64_t id, const std::string& terminalReason, const std::string& terminalError);
    std::vector<DeadPendingFieldEdit> LoadDeadPendingFieldEdits();
    void DeleteDeadPendingFieldEdit(std::int64_t deadId);

  private:
    SQLite::Database db;

    // Cached prepared statements for the hot paths (SaveTicket / TryGetTicket).
    // Lazily initialised on first use; reset+clearBindings before each bind cycle.
    //
    // SQLite::Statement (and the underlying sqlite3_stmt) is NOT thread-safe per
    // <https://sqlite.org/threadsafe.html> — even when the connection itself was
    // opened with OPEN_FULLMUTEX (which serialises sqlite3_step calls on the
    // connection), two threads sharing the same prepared statement still race on
    // bound parameter state and result-row cursors. SaveTicket / TryGetTicket
    // are called from both the UI thread and the Lua automation worker thread
    // (via LuaGetTicketBind), so we serialise every cached-statement-using
    // method through stmtMutex_ below.
    mutable std::mutex stmtMutex_;
    SQLite::Statement& stmt(std::unique_ptr<SQLite::Statement>& slot, const char* sql);

    std::unique_ptr<SQLite::Statement> stmt_save_upsert_ticket_;
    std::unique_ptr<SQLite::Statement> stmt_save_delete_fields_;
    std::unique_ptr<SQLite::Statement> stmt_save_insert_field_;
    std::unique_ptr<SQLite::Statement> stmt_save_delete_rich_;
    std::unique_ptr<SQLite::Statement> stmt_save_insert_rich_;
    std::unique_ptr<SQLite::Statement> stmt_get_exists_;
    std::unique_ptr<SQLite::Statement> stmt_get_fields_;
    std::unique_ptr<SQLite::Statement> stmt_get_rich_;
};
