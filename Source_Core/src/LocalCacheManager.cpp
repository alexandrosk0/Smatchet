#include "LocalCacheManager.h"
#include "Logger.h"

#include <chrono>
#include <exception>
#include <stdexcept>

LocalCacheManager::LocalCacheManager(const std::string& dbPath)
    : db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX) {
    LOG_INFO("LocalCacheManager: opening db path=%s", dbPath.c_str());
    // WAL improves crash safety and allows readers while a writer is active.
    // synchronous=NORMAL is the recommended pairing with WAL (fsync on checkpoint, not every commit).
    try {
        db.exec("PRAGMA journal_mode=WAL");
        db.exec("PRAGMA synchronous=NORMAL");
        db.setBusyTimeout(5000);
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager: failed to set WAL pragmas: %s", ex.what());
    }
    db.exec("CREATE TABLE IF NOT EXISTS tickets (id TEXT PRIMARY KEY)");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_values ("
            "ticket_id TEXT NOT NULL, "
            "field_key TEXT NOT NULL, "
            "field_value TEXT, "
            "PRIMARY KEY(ticket_id, field_key))");
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "payload TEXT NOT NULL, "
            "attempts INTEGER NOT NULL DEFAULT 0, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_creates_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "original_id INTEGER NOT NULL, "
            "payload TEXT NOT NULL, "
            "attempts INTEGER NOT NULL, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL, "
            "archived_at INTEGER NOT NULL, "
            "terminal_reason TEXT NOT NULL)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_pending_creates_dead_archived_at "
            "ON pending_creates_dead(archived_at DESC)");
    db.exec("CREATE TABLE IF NOT EXISTS cache_meta ("
            "key TEXT PRIMARY KEY, "
            "value TEXT NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "issue_key TEXT NOT NULL, "
            "field_id TEXT NOT NULL, "
            "fields_payload_json TEXT NOT NULL, "
            "attempts INTEGER NOT NULL DEFAULT 0, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL)");
    db.exec("CREATE TABLE IF NOT EXISTS pending_field_edits_dead ("
            "dead_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "original_id INTEGER NOT NULL, "
            "issue_key TEXT NOT NULL, "
            "field_id TEXT NOT NULL, "
            "fields_payload_json TEXT NOT NULL, "
            "attempts INTEGER NOT NULL, "
            "last_error TEXT, "
            "created_at INTEGER NOT NULL, "
            "archived_at INTEGER NOT NULL, "
            "terminal_reason TEXT NOT NULL)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_pending_field_edits_dead_archived_at "
            "ON pending_field_edits_dead(archived_at DESC)");
    LOG_INFO("LocalCacheManager: schema ready");
}

void LocalCacheManager::SaveTicket(const CachedTicket& ticket) {
    try {
        SQLite::Transaction transaction(db);

        SQLite::Statement ticketUpsert(db, "INSERT OR REPLACE INTO tickets (id) VALUES (?)");
        ticketUpsert.bind(1, ticket.id);
        ticketUpsert.exec();

        // Keep selected field cache rows in sync with latest snapshot for this ticket.
        SQLite::Statement deleteFields(db, "DELETE FROM ticket_field_values WHERE ticket_id = ?");
        deleteFields.bind(1, ticket.id);
        deleteFields.exec();

        SQLite::Statement fieldUpsert(
            db, "INSERT OR REPLACE INTO ticket_field_values (ticket_id, field_key, field_value) VALUES (?, ?, ?)");
        for (const auto& kv : ticket.fieldValues) {
            fieldUpsert.bind(1, ticket.id);
            fieldUpsert.bind(2, kv.first);
            fieldUpsert.bind(3, kv.second);
            fieldUpsert.exec();
            fieldUpsert.reset();
            fieldUpsert.clearBindings();
        }

        transaction.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::SaveTicket failed ticket=%s err=%s", ticket.id.c_str(), ex.what());
        throw;
    }
}

bool LocalCacheManager::TryGetTicket(const std::string& ticketId, CachedTicket& out) {
    out = CachedTicket{};
    out.id = ticketId;
    try {
        SQLite::Statement exists(db, "SELECT 1 FROM tickets WHERE id = ? LIMIT 1");
        exists.bind(1, ticketId);
        if (!exists.executeStep()) {
            return false;
        }
        SQLite::Statement fieldQuery(db, "SELECT field_key, field_value FROM ticket_field_values WHERE ticket_id = ?");
        fieldQuery.bind(1, ticketId);
        while (fieldQuery.executeStep()) {
            const std::string fieldKey = fieldQuery.getColumn(0).getText();
            out.fieldValues[fieldKey] =
                fieldQuery.getColumn(1).isNull() ? std::string() : std::string(fieldQuery.getColumn(1).getText());
        }
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::TryGetTicket failed ticket=%s err=%s", ticketId.c_str(), ex.what());
        throw;
    }
}

void LocalCacheManager::DeleteTicket(const std::string& ticketId) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement deleteFields(db, "DELETE FROM ticket_field_values WHERE ticket_id = ?");
        deleteFields.bind(1, ticketId);
        deleteFields.exec();
        SQLite::Statement deleteTicket(db, "DELETE FROM tickets WHERE id = ?");
        deleteTicket.bind(1, ticketId);
        deleteTicket.exec();
        transaction.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteTicket failed ticket=%s err=%s", ticketId.c_str(), ex.what());
        throw;
    }
}

std::vector<CachedTicket> LocalCacheManager::GetAllTickets() {
    try {
        std::vector<CachedTicket> results;
        SQLite::Statement query(db, "SELECT id FROM tickets");
        while (query.executeStep()) {
            CachedTicket ticket;
            ticket.id = query.getColumn(0).getText();
            results.push_back(ticket);
        }

        std::unordered_map<std::string, size_t> indexById;
        for (size_t i = 0; i < results.size(); ++i) {
            indexById[results[i].id] = i;
        }

        size_t orphanRows = 0;
        SQLite::Statement fieldQuery(db, "SELECT ticket_id, field_key, field_value FROM ticket_field_values");
        while (fieldQuery.executeStep()) {
            const std::string ticketId = fieldQuery.getColumn(0).getText();
            const auto it = indexById.find(ticketId);
            if (it == indexById.end()) {
                ++orphanRows;
                continue;
            }

            const std::string fieldKey = fieldQuery.getColumn(1).getText();
            const std::string fieldValue =
                fieldQuery.getColumn(2).isNull() ? std::string() : std::string(fieldQuery.getColumn(2).getText());
            results[it->second].fieldValues[fieldKey] = fieldValue;
        }
        if (orphanRows > 0) {
            LOG_WARN("LocalCacheManager::GetAllTickets ignored orphan field rows=%zu", orphanRows);
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetAllTickets failed err=%s", ex.what());
        throw;
    }
}

std::vector<std::string> LocalCacheManager::GetAllTicketIds() {
    try {
        std::vector<std::string> results;
        SQLite::Statement query(db, "SELECT id FROM tickets");
        while (query.executeStep()) {
            results.push_back(query.getColumn(0).getText());
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetAllTicketIds failed err=%s", ex.what());
        throw;
    }
}

std::int64_t LocalCacheManager::EnqueuePendingCreate(const std::string& payload) {
    try {
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement insert(
            db, "INSERT INTO pending_creates (payload, attempts, last_error, created_at) VALUES (?, 0, '', ?)");
        insert.bind(1, payload);
        insert.bind(2, epoch);
        insert.exec();
        return db.getLastInsertRowid();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::EnqueuePendingCreate failed err=%s", ex.what());
        throw;
    }
}

std::vector<PendingCreate> LocalCacheManager::LoadPendingCreates() {
    try {
        std::vector<PendingCreate> results;
        SQLite::Statement query(
            db, "SELECT id, payload, attempts, last_error, created_at FROM pending_creates ORDER BY id ASC");
        while (query.executeStep()) {
            PendingCreate pc;
            pc.Id = query.getColumn(0).getInt64();
            pc.Payload = query.getColumn(1).getText();
            pc.Attempts = query.getColumn(2).getInt();
            pc.LastError = query.getColumn(3).isNull() ? std::string() : query.getColumn(3).getText();
            pc.CreatedAtEpochSec = query.getColumn(4).getInt64();
            results.push_back(std::move(pc));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadPendingCreates failed err=%s", ex.what());
        throw;
    }
}

void LocalCacheManager::UpdatePendingCreate(std::int64_t id, int attempts, const std::string& lastError) {
    try {
        SQLite::Statement update(db, "UPDATE pending_creates SET attempts = ?, last_error = ? WHERE id = ?");
        update.bind(1, attempts);
        update.bind(2, lastError);
        update.bind(3, id);
        update.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::UpdatePendingCreate failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::DeletePendingCreate(std::int64_t id) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_creates WHERE id = ?");
        del.bind(1, id);
        del.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeletePendingCreate failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::ArchivePendingCreate(std::int64_t id, const std::string& terminalReason,
                                             const std::string& terminalError) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement select(db,
                                 "SELECT payload, attempts, last_error, created_at FROM pending_creates WHERE id = ?");
        select.bind(1, id);
        if (!select.executeStep()) {
            throw std::runtime_error("pending_create row not found");
        }
        const std::string payload =
            select.getColumn(0).isNull() ? std::string() : std::string(select.getColumn(0).getText());
        const int attempts = select.getColumn(1).getInt();
        const std::string lastError =
            !terminalError.empty()
                ? terminalError
                : (select.getColumn(2).isNull() ? std::string() : std::string(select.getColumn(2).getText()));
        const std::int64_t createdAtEpochSec = select.getColumn(3).getInt64();
        const std::int64_t archivedAtEpochSec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        SQLite::Statement insert(
            db, "INSERT INTO pending_creates_dead "
                "(original_id, payload, attempts, last_error, created_at, archived_at, terminal_reason) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, id);
        insert.bind(2, payload);
        insert.bind(3, attempts);
        insert.bind(4, lastError);
        insert.bind(5, createdAtEpochSec);
        insert.bind(6, archivedAtEpochSec);
        insert.bind(7, terminalReason);
        insert.exec();

        SQLite::Statement del(db, "DELETE FROM pending_creates WHERE id = ?");
        del.bind(1, id);
        del.exec();
        transaction.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::ArchivePendingCreate failed id=%lld reason=%s err=%s", static_cast<long long>(id),
                  terminalReason.c_str(), ex.what());
        throw;
    }
}

std::vector<DeadPendingCreate> LocalCacheManager::LoadDeadPendingCreates() {
    try {
        std::vector<DeadPendingCreate> results;
        SQLite::Statement query(
            db, "SELECT dead_id, original_id, payload, attempts, last_error, created_at, archived_at, terminal_reason "
                "FROM pending_creates_dead ORDER BY archived_at DESC, dead_id DESC");
        while (query.executeStep()) {
            DeadPendingCreate row;
            row.DeadId = query.getColumn(0).getInt64();
            row.OriginalId = query.getColumn(1).getInt64();
            row.Payload = query.getColumn(2).isNull() ? std::string() : std::string(query.getColumn(2).getText());
            row.Attempts = query.getColumn(3).getInt();
            row.LastError = query.getColumn(4).isNull() ? std::string() : std::string(query.getColumn(4).getText());
            row.CreatedAtEpochSec = query.getColumn(5).getInt64();
            row.ArchivedAtEpochSec = query.getColumn(6).getInt64();
            row.TerminalReason =
                query.getColumn(7).isNull() ? std::string() : std::string(query.getColumn(7).getText());
            results.push_back(std::move(row));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadDeadPendingCreates failed err=%s", ex.what());
        throw;
    }
}

size_t LocalCacheManager::GetDeadPendingCreateCount() {
    try {
        SQLite::Statement countQuery(db, "SELECT COUNT(*) FROM pending_creates_dead");
        if (!countQuery.executeStep()) {
            return 0;
        }
        const int count = countQuery.getColumn(0).getInt();
        return count > 0 ? static_cast<size_t>(count) : 0u;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::GetDeadPendingCreateCount failed err=%s", ex.what());
        throw;
    }
}

size_t LocalCacheManager::RunOneTimeLegacyDropPendingAtMaxAttempts() {
    constexpr const char* kKey = "legacy_drop_pending_ge_max_attempts_v1";
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement probe(db, "SELECT 1 FROM cache_meta WHERE key = ? LIMIT 1");
        probe.bind(1, kKey);
        if (probe.executeStep()) {
            transaction.commit();
            return 0;
        }
        SQLite::Statement del(db, "DELETE FROM pending_creates WHERE attempts >= ?");
        del.bind(1, OfflineCreateQueue::kMaxReplayAttempts);
        del.exec();
        const int deleted = db.getChanges();
        SQLite::Statement ins(db, "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, '1')");
        ins.bind(1, kKey);
        ins.exec();
        transaction.commit();
        if (deleted > 0) {
            LOG_INFO("LocalCacheManager::RunOneTimeLegacyDropPendingAtMaxAttempts dropped rows=%d", deleted);
        }
        return deleted > 0 ? static_cast<size_t>(deleted) : 0u;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RunOneTimeLegacyDropPendingAtMaxAttempts failed err=%s", ex.what());
        throw;
    }
}

bool LocalCacheManager::RestoreDeadPendingCreate(const std::int64_t originalPendingId) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement sel(db, "SELECT dead_id, payload FROM pending_creates_dead WHERE original_id = ? "
                                  "ORDER BY dead_id DESC LIMIT 1");
        sel.bind(1, originalPendingId);
        if (!sel.executeStep()) {
            transaction.commit();
            return false;
        }
        const std::int64_t deadId = sel.getColumn(0).getInt64();
        const std::string payload = sel.getColumn(1).isNull() ? std::string() : std::string(sel.getColumn(1).getText());
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement ins(
            db, "INSERT INTO pending_creates (payload, attempts, last_error, created_at) VALUES (?, 0, '', ?)");
        ins.bind(1, payload);
        ins.bind(2, epoch);
        ins.exec();
        SQLite::Statement delDead(db, "DELETE FROM pending_creates_dead WHERE dead_id = ?");
        delDead.bind(1, deadId);
        delDead.exec();
        transaction.commit();
        LOG_INFO("LocalCacheManager::RestoreDeadPendingCreate original_id=%lld dead_id=%lld",
                 static_cast<long long>(originalPendingId), static_cast<long long>(deadId));
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RestoreDeadPendingCreate failed original_id=%lld err=%s",
                  static_cast<long long>(originalPendingId), ex.what());
        throw;
    }
}

void LocalCacheManager::DeleteDeadPendingCreate(const std::int64_t deadId) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_creates_dead WHERE dead_id = ?");
        del.bind(1, deadId);
        del.exec();
        if (db.getChanges() == 0) {
            LOG_WARN("LocalCacheManager::DeleteDeadPendingCreate: no row for dead_id=%lld",
                     static_cast<long long>(deadId));
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteDeadPendingCreate failed dead_id=%lld err=%s",
                  static_cast<long long>(deadId), ex.what());
        throw;
    }
}

std::int64_t LocalCacheManager::EnqueuePendingFieldEdit(const std::string& issueKey, const std::string& fieldId,
                                                        const std::string& fieldsPayloadJson) {
    try {
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement insert(
            db, "INSERT INTO pending_field_edits (issue_key, field_id, fields_payload_json, attempts, last_error, "
                "created_at) VALUES (?, ?, ?, 0, '', ?)");
        insert.bind(1, issueKey);
        insert.bind(2, fieldId);
        insert.bind(3, fieldsPayloadJson);
        insert.bind(4, epoch);
        insert.exec();
        return db.getLastInsertRowid();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::EnqueuePendingFieldEdit failed err=%s", ex.what());
        throw;
    }
}

std::vector<PendingFieldEditRecord> LocalCacheManager::LoadPendingFieldEdits() {
    try {
        std::vector<PendingFieldEditRecord> results;
        SQLite::Statement query(db,
                                "SELECT id, issue_key, field_id, fields_payload_json, attempts, last_error, created_at "
                                "FROM pending_field_edits ORDER BY id ASC");
        while (query.executeStep()) {
            PendingFieldEditRecord row;
            row.Id = query.getColumn(0).getInt64();
            row.IssueKey = query.getColumn(1).isNull() ? std::string() : std::string(query.getColumn(1).getText());
            row.FieldId = query.getColumn(2).isNull() ? std::string() : std::string(query.getColumn(2).getText());
            row.FieldsPayloadJson =
                query.getColumn(3).isNull() ? std::string() : std::string(query.getColumn(3).getText());
            row.Attempts = query.getColumn(4).getInt();
            row.LastError = query.getColumn(5).isNull() ? std::string() : std::string(query.getColumn(5).getText());
            row.CreatedAtEpochSec = query.getColumn(6).getInt64();
            results.push_back(std::move(row));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadPendingFieldEdits failed err=%s", ex.what());
        throw;
    }
}

void LocalCacheManager::UpdatePendingFieldEdit(std::int64_t id, int attempts, const std::string& lastError) {
    try {
        SQLite::Statement update(db, "UPDATE pending_field_edits SET attempts = ?, last_error = ? WHERE id = ?");
        update.bind(1, attempts);
        update.bind(2, lastError);
        update.bind(3, id);
        update.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::UpdatePendingFieldEdit failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::DeletePendingFieldEdit(std::int64_t id) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_field_edits WHERE id = ?");
        del.bind(1, id);
        del.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeletePendingFieldEdit failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::ArchivePendingFieldEdit(std::int64_t id, const std::string& terminalReason,
                                                const std::string& terminalError) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement select(
            db, "SELECT issue_key, field_id, fields_payload_json, attempts, last_error, created_at FROM "
                "pending_field_edits WHERE id = ?");
        select.bind(1, id);
        if (!select.executeStep()) {
            throw std::runtime_error("pending_field_edits row not found");
        }
        const std::string issueKey =
            select.getColumn(0).isNull() ? std::string() : std::string(select.getColumn(0).getText());
        const std::string fieldId =
            select.getColumn(1).isNull() ? std::string() : std::string(select.getColumn(1).getText());
        const std::string payload =
            select.getColumn(2).isNull() ? std::string() : std::string(select.getColumn(2).getText());
        const int attempts = select.getColumn(3).getInt();
        const std::string lastError =
            !terminalError.empty()
                ? terminalError
                : (select.getColumn(4).isNull() ? std::string() : std::string(select.getColumn(4).getText()));
        const std::int64_t createdAtEpochSec = select.getColumn(5).getInt64();
        const std::int64_t archivedAtEpochSec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        SQLite::Statement insert(
            db,
            "INSERT INTO pending_field_edits_dead "
            "(original_id, issue_key, field_id, fields_payload_json, attempts, last_error, created_at, archived_at, "
            "terminal_reason) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, id);
        insert.bind(2, issueKey);
        insert.bind(3, fieldId);
        insert.bind(4, payload);
        insert.bind(5, attempts);
        insert.bind(6, lastError);
        insert.bind(7, createdAtEpochSec);
        insert.bind(8, archivedAtEpochSec);
        insert.bind(9, terminalReason);
        insert.exec();

        SQLite::Statement del(db, "DELETE FROM pending_field_edits WHERE id = ?");
        del.bind(1, id);
        del.exec();
        transaction.commit();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::ArchivePendingFieldEdit failed id=%lld reason=%s err=%s",
                  static_cast<long long>(id), terminalReason.c_str(), ex.what());
        throw;
    }
}

std::vector<DeadPendingFieldEdit> LocalCacheManager::LoadDeadPendingFieldEdits() {
    try {
        std::vector<DeadPendingFieldEdit> results;
        SQLite::Statement query(
            db, "SELECT dead_id, original_id, issue_key, field_id, fields_payload_json, attempts, last_error, "
                "created_at, archived_at, terminal_reason "
                "FROM pending_field_edits_dead ORDER BY archived_at DESC, dead_id DESC");
        while (query.executeStep()) {
            DeadPendingFieldEdit row;
            row.DeadId = query.getColumn(0).getInt64();
            row.OriginalId = query.getColumn(1).getInt64();
            row.IssueKey = query.getColumn(2).isNull() ? std::string() : std::string(query.getColumn(2).getText());
            row.FieldId = query.getColumn(3).isNull() ? std::string() : std::string(query.getColumn(3).getText());
            row.FieldsPayloadJson =
                query.getColumn(4).isNull() ? std::string() : std::string(query.getColumn(4).getText());
            row.Attempts = query.getColumn(5).getInt();
            row.LastError = query.getColumn(6).isNull() ? std::string() : std::string(query.getColumn(6).getText());
            row.CreatedAtEpochSec = query.getColumn(7).getInt64();
            row.ArchivedAtEpochSec = query.getColumn(8).getInt64();
            row.TerminalReason =
                query.getColumn(9).isNull() ? std::string() : std::string(query.getColumn(9).getText());
            results.push_back(std::move(row));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadDeadPendingFieldEdits failed err=%s", ex.what());
        throw;
    }
}

void LocalCacheManager::DeleteDeadPendingFieldEdit(const std::int64_t deadId) {
    try {
        SQLite::Statement del(db, "DELETE FROM pending_field_edits_dead WHERE dead_id = ?");
        del.bind(1, deadId);
        del.exec();
        if (db.getChanges() == 0) {
            LOG_WARN("LocalCacheManager::DeleteDeadPendingFieldEdit: no row for dead_id=%lld",
                     static_cast<long long>(deadId));
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::DeleteDeadPendingFieldEdit failed dead_id=%lld err=%s",
                  static_cast<long long>(deadId), ex.what());
        throw;
    }
}






