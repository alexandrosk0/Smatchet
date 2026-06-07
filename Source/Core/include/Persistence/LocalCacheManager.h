#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CachedTicketTypes.h"
#include "OfflineQueueReplayPolicy.h"

#if defined(SMATCHET_WITH_AI)
#include "AiTypes.h"
#endif

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

    // Ticket storage is namespaced by `backendKey` (the NormalizeViewsBackendKey output for the
    // owning backend — multi-grid Slice 1b, ADR-0018 decision 4). All ticket methods read/write
    // the `tickets_v2` table family whose PK is (backend_key, id); the legacy v1 tables are
    // retained on disk unused (Persistence additive-only invariant — no DROP/RENAME).
    void SaveTicket(const std::string& backendKey, const CachedTicket& ticket);
    /** Persist a batch of tickets in a SINGLE transaction (vs one per ticket). Used by the
     *  streaming-sync apply loop to coalesce up to 20 per-frame commits into one. See
     *  docs/plans/shipped/memory-budget-and-lifetime-hardening.md § Phase 3(a). */
    void SaveTickets(const std::string& backendKey, const std::vector<CachedTicket>& tickets);
    /** @return false if `ticketId` is not present under `backendKey` in `tickets_v2`. */
    bool TryGetTicket(const std::string& backendKey, const std::string& ticketId, CachedTicket& out);
    void DeleteTicket(const std::string& backendKey, const std::string& ticketId);
    std::vector<CachedTicket> GetAllTickets(const std::string& backendKey);
    /** Streaming overload — calls `fn` once per fully-populated ticket instead of materialising the
     *  entire result set. Avoids the O(N) in-memory join for bulk-sync paths (§4.3). */
    void ForEachTicket(const std::string& backendKey, const std::function<void(CachedTicket&&)>& fn);
    std::vector<std::string> GetAllTicketIds(const std::string& backendKey);

    /** One-time copy migration of the legacy un-namespaced ticket tables (`tickets`,
     * `ticket_field_values`, `ticket_field_rich_values`) into the `*_v2` family, stamping
     * every copied row with `backendKey` (legacy rows were necessarily cached against the
     * then-only configured backend). Gated on a `cache_meta` flag so it runs at most once per
     * database file; legacy tables are retained on disk unused (additive-only invariant).
     * No-op (flag untouched) when `backendKey` is empty. @return number of ticket rows copied.
     */
    size_t RunOneTimeTicketsV2CopyMigration(const std::string& backendKey);

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
                                         const std::string& originalRichValue = std::string(),
                                         const std::string& originalValue = std::string(),
                                         bool hasOriginalValue = false);
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

#if defined(SMATCHET_WITH_AI)
    // ---------------- AI chat persistence (Phase 3 of ai-chat-claude-desktop-parity) ----------------
    // All writes routed through `smatchet::ai::chat_persist` worker so the UI thread
    // never blocks on SQLite. Schema is additive (`CREATE TABLE IF NOT EXISTS`); old
    // DBs auto-upgrade without a schema-version probe. Failures degrade to in-memory-
    // only — every call wraps in try/catch and logs `LOG_WARN` on failure. Pinned rows
    // are exempt from `TrimChatMessages` so user-bookmarked context survives growth.
    /// @return newly-inserted row id, or -1 on failure.
    std::int64_t AppendChatMessage(const AiMessage& msg);
    /// Flips the `pinned` column for `id`. No-op + WARN if `id` is absent.
    void UpdateChatMessagePin(std::int64_t id, bool pinned);
    /// Loads up to `cap` most-recent rows (sorted ascending by id so the UI displays
    /// in chronological order). `outMessages[i]` and `outIds[i]` are parallel — the
    /// row id at the same index identifies the SQLite row for that message.
    void LoadChatMessages(std::size_t cap, std::vector<AiMessage>& outMessages, std::vector<std::int64_t>& outIds);
    /// Removes every row from the chat table (user-driven Clear Chat).
    void ClearChatMessages();
    /// Keeps the most-recent `maxRows` non-pinned rows + every pinned row. Idempotent.
    void TrimChatMessages(std::size_t maxRows);
#endif

  private:
    /// Write one ticket's rows via the cached prepared statements. Caller holds `stmtMutex_`
    /// and owns the enclosing SQLite::Transaction (SaveTicket / SaveTickets). See the .cpp.
    void writeTicketRows_(const std::string& backendKey, const CachedTicket& ticket);

    SQLite::Database db;

    // Cached prepared statements for the hot paths (SaveTicket / TryGetTicket).
    // Lazily initialised on first use; reset+clearBindings before each bind cycle.
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
