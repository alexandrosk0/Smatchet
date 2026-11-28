// LocalCacheManager — the offline write queues: `pending_creates` and `pending_field_edits` with their
// dead-letter twins. Split from LocalCacheManager.cpp as a pure move (no logic change) to keep both
// translation units under the line ceiling; the schema, pragmas, ticket rows and the one-time migrations
// stay in LocalCacheManager.cpp.

#include "LocalCacheManager.h"

#include "IssueDraft.h"
#include "Logger.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

std::int64_t LocalCacheManager::EnqueuePendingCreate(const std::string& backendKey, const std::string& payload) {
    try {
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement insert(db, "INSERT INTO pending_creates (payload, backend_key, attempts, last_error, "
                                     "created_at) VALUES (?, ?, 0, '', ?)");
        insert.bind(1, payload);
        insert.bind(2, backendKey);
        insert.bind(3, epoch);
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
        SQLite::Statement query(db, "SELECT id, payload, attempts, last_error, created_at, backend_key "
                                    "FROM pending_creates ORDER BY id ASC");
        while (query.executeStep()) {
            PendingCreate pc;
            pc.Id = query.getColumn(0).getInt64();
            pc.Payload = query.getColumn(1).getText();
            pc.Attempts = query.getColumn(2).getInt();
            pc.LastError = query.getColumn(3).isNull() ? std::string() : query.getColumn(3).getText();
            pc.CreatedAtEpochSec = query.getColumn(4).getInt64();
            pc.BackendKey = query.getColumn(5).isNull() ? std::string() : query.getColumn(5).getText();
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

void LocalCacheManager::UpdatePendingCreatePayload(std::int64_t id, const std::string& payload) {
    try {
        SQLite::Statement update(db, "UPDATE pending_creates SET payload = ? WHERE id = ?");
        update.bind(1, payload);
        update.bind(2, id);
        update.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::UpdatePendingCreatePayload failed id=%lld err=%s", static_cast<long long>(id),
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
        SQLite::Statement select(
            db, "SELECT payload, attempts, last_error, created_at, backend_key FROM pending_creates WHERE id = ?");
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
        const std::string backendKey =
            select.getColumn(4).isNull() ? std::string() : std::string(select.getColumn(4).getText());
        const std::int64_t archivedAtEpochSec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        SQLite::Statement insert(
            db, "INSERT INTO pending_creates_dead "
                "(original_id, payload, backend_key, attempts, last_error, created_at, archived_at, terminal_reason) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, id);
        insert.bind(2, payload);
        insert.bind(3, backendKey);
        insert.bind(4, attempts);
        insert.bind(5, lastError);
        insert.bind(6, createdAtEpochSec);
        insert.bind(7, archivedAtEpochSec);
        insert.bind(8, terminalReason);
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
        SQLite::Statement query(db,
                                "SELECT dead_id, original_id, payload, attempts, last_error, created_at, archived_at, "
                                "terminal_reason, backend_key "
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
            row.BackendKey = query.getColumn(8).isNull() ? std::string() : std::string(query.getColumn(8).getText());
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

bool LocalCacheManager::RestoreDeadPendingCreate(const std::int64_t originalPendingId) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement sel(db, "SELECT dead_id, payload, backend_key FROM pending_creates_dead "
                                  "WHERE original_id = ? ORDER BY dead_id DESC LIMIT 1");
        sel.bind(1, originalPendingId);
        if (!sel.executeStep()) {
            transaction.commit();
            return false;
        }
        const std::int64_t deadId = sel.getColumn(0).getInt64();
        const std::string archivedPayload =
            sel.getColumn(1).isNull() ? std::string() : std::string(sel.getColumn(1).getText());
        // Fresh-create scrub applied to the archived payload INSIDE this transaction (the
        // service used to pre-load + scrub outside it — an exception there could skip the
        // scrub and restore a stale ExistingIssueKey verbatim). Unparseable payloads restore
        // verbatim; the replay tick's parse path terminally dead-letters real garbage.
        std::string parseErr;
        const std::string payload = IssueDraftHelpers::ScrubFreshCreatePayload(archivedPayload, parseErr);
        if (!parseErr.empty()) {
            LOG_WARN("LocalCacheManager::RestoreDeadPendingCreate original_id=%lld payload parse failed (%s) — "
                     "restoring verbatim",
                     static_cast<long long>(originalPendingId), parseErr.c_str());
        }
        // Restore re-queues under the SAME backend the row was originally queued against
        // (multi-grid Slice 1c) — never the currently-focused context's key.
        const std::string backendKey =
            sel.getColumn(2).isNull() ? std::string() : std::string(sel.getColumn(2).getText());
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement ins(db, "INSERT INTO pending_creates (payload, backend_key, attempts, last_error, "
                                  "created_at) VALUES (?, ?, 0, '', ?)");
        ins.bind(1, payload);
        ins.bind(2, backendKey);
        ins.bind(3, epoch);
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

std::int64_t LocalCacheManager::EnqueuePendingFieldEdit(const std::string& backendKey, const std::string& issueKey,
                                                        const std::string& fieldId,
                                                        const std::string& fieldsPayloadJson,
                                                        const std::string& originalRichValue,
                                                        const std::string& originalValue, bool hasOriginalValue) {
    try {
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement insert(db, "INSERT INTO pending_field_edits (issue_key, field_id, fields_payload_json, "
                                     "original_rich_value, original_value, has_original_value, backend_key, attempts, "
                                     "last_error, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, 0, '', ?)");
        insert.bind(1, issueKey);
        insert.bind(2, fieldId);
        insert.bind(3, fieldsPayloadJson);
        if (originalRichValue.empty())
            insert.bind(4); // NULL
        else
            insert.bind(4, originalRichValue);
        // original_value is stored NULL when blank to save space, but has_original_value records
        // whether a base was CAPTURED (presence) independent of its emptiness (ADR-0016).
        if (originalValue.empty())
            insert.bind(5); // NULL
        else
            insert.bind(5, originalValue);
        insert.bind(6, hasOriginalValue ? 1 : 0);
        insert.bind(7, backendKey);
        insert.bind(8, epoch);
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
        SQLite::Statement query(db, "SELECT id, issue_key, field_id, fields_payload_json, "
                                    "COALESCE(original_rich_value, ''), "
                                    "COALESCE(has_merge_conflict, 0), COALESCE(conflict_context_json, ''), "
                                    "attempts, last_error, created_at, COALESCE(original_value, ''), "
                                    "COALESCE(has_original_value, 0), COALESCE(backend_key, '') "
                                    "FROM pending_field_edits ORDER BY id ASC");
        while (query.executeStep()) {
            PendingFieldEditRecord row;
            row.Id = query.getColumn(0).getInt64();
            row.IssueKey = query.getColumn(1).isNull() ? std::string() : std::string(query.getColumn(1).getText());
            row.FieldId = query.getColumn(2).isNull() ? std::string() : std::string(query.getColumn(2).getText());
            row.FieldsPayloadJson =
                query.getColumn(3).isNull() ? std::string() : std::string(query.getColumn(3).getText());
            row.OriginalRichValue =
                query.getColumn(4).isNull() ? std::string() : std::string(query.getColumn(4).getText());
            row.HasMergeConflict = query.getColumn(5).getInt() != 0;
            row.ConflictContextJson =
                query.getColumn(6).isNull() ? std::string() : std::string(query.getColumn(6).getText());
            row.Attempts = query.getColumn(7).getInt();
            row.LastError = query.getColumn(8).isNull() ? std::string() : std::string(query.getColumn(8).getText());
            row.CreatedAtEpochSec = query.getColumn(9).getInt64();
            row.OriginalValue =
                query.getColumn(10).isNull() ? std::string() : std::string(query.getColumn(10).getText());
            row.HasOriginalValue = query.getColumn(11).getInt() != 0;
            row.BackendKey = query.getColumn(12).isNull() ? std::string() : std::string(query.getColumn(12).getText());
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

void LocalCacheManager::MarkFieldEditConflict(std::int64_t id, const std::string& contextJson) {
    try {
        SQLite::Statement upd(
            db, "UPDATE pending_field_edits SET has_merge_conflict = 1, conflict_context_json = ? WHERE id = ?");
        upd.bind(1, contextJson);
        upd.bind(2, id);
        upd.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::MarkFieldEditConflict failed id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
        throw;
    }
}

void LocalCacheManager::ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedPayloadJson) {
    try {
        SQLite::Statement upd(db,
                              "UPDATE pending_field_edits SET has_merge_conflict = 0, conflict_context_json = NULL, "
                              "original_rich_value = NULL, original_value = NULL, has_original_value = 0, "
                              "fields_payload_json = ? WHERE id = ?");
        upd.bind(1, resolvedPayloadJson);
        upd.bind(2, id);
        upd.exec();
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::ResolveFieldEditConflict failed id=%lld err=%s", static_cast<long long>(id),
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
        SQLite::Statement select(db, "SELECT issue_key, field_id, fields_payload_json, "
                                     "COALESCE(original_rich_value,''), attempts, last_error, created_at, "
                                     "COALESCE(original_value,''), COALESCE(has_original_value, 0), "
                                     "COALESCE(backend_key, '') FROM "
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
        const std::string originalRichValue =
            select.getColumn(3).isNull() ? std::string() : std::string(select.getColumn(3).getText());
        const int attempts = select.getColumn(4).getInt();
        const std::string lastError =
            !terminalError.empty()
                ? terminalError
                : (select.getColumn(5).isNull() ? std::string() : std::string(select.getColumn(5).getText()));
        const std::int64_t createdAtEpochSec = select.getColumn(6).getInt64();
        const std::string originalValue =
            select.getColumn(7).isNull() ? std::string() : std::string(select.getColumn(7).getText());
        const bool hasOriginalValue = select.getColumn(8).getInt() != 0;
        const std::string backendKey =
            select.getColumn(9).isNull() ? std::string() : std::string(select.getColumn(9).getText());
        const std::int64_t archivedAtEpochSec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        SQLite::Statement insert(db,
                                 "INSERT INTO pending_field_edits_dead "
                                 "(original_id, issue_key, field_id, fields_payload_json, original_rich_value, "
                                 "original_value, has_original_value, backend_key, attempts, last_error, created_at, "
                                 "archived_at, terminal_reason) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        insert.bind(1, id);
        insert.bind(2, issueKey);
        insert.bind(3, fieldId);
        insert.bind(4, payload);
        if (originalRichValue.empty())
            insert.bind(5);
        else
            insert.bind(5, originalRichValue);
        if (originalValue.empty())
            insert.bind(6);
        else
            insert.bind(6, originalValue);
        insert.bind(7, hasOriginalValue ? 1 : 0);
        insert.bind(8, backendKey);
        insert.bind(9, attempts);
        insert.bind(10, lastError);
        insert.bind(11, createdAtEpochSec);
        insert.bind(12, archivedAtEpochSec);
        insert.bind(13, terminalReason);
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
                "created_at, archived_at, terminal_reason, COALESCE(backend_key, '') "
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
            row.BackendKey = query.getColumn(10).isNull() ? std::string() : std::string(query.getColumn(10).getText());
            results.push_back(std::move(row));
        }
        return results;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::LoadDeadPendingFieldEdits failed err=%s", ex.what());
        throw;
    }
}

bool LocalCacheManager::RestoreDeadPendingFieldEdit(const std::int64_t originalPendingId) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement sel(db, "SELECT dead_id, issue_key, field_id, fields_payload_json, original_rich_value, "
                                  "original_value, COALESCE(has_original_value, 0), COALESCE(backend_key, '') "
                                  "FROM pending_field_edits_dead WHERE original_id = ? "
                                  "ORDER BY dead_id DESC LIMIT 1");
        sel.bind(1, originalPendingId);
        if (!sel.executeStep()) {
            transaction.commit();
            return false;
        }
        const std::int64_t deadId = sel.getColumn(0).getInt64();
        const std::string issueKey =
            sel.getColumn(1).isNull() ? std::string() : std::string(sel.getColumn(1).getText());
        const std::string fieldId = sel.getColumn(2).isNull() ? std::string() : std::string(sel.getColumn(2).getText());
        const std::string payload = sel.getColumn(3).isNull() ? std::string() : std::string(sel.getColumn(3).getText());
        const bool richIsNull = sel.getColumn(4).isNull();
        const std::string originalRichValue = richIsNull ? std::string() : std::string(sel.getColumn(4).getText());
        const bool valueIsNull = sel.getColumn(5).isNull();
        const std::string originalValue = valueIsNull ? std::string() : std::string(sel.getColumn(5).getText());
        const bool hasOriginalValue = sel.getColumn(6).getInt() != 0;
        // Restore re-queues under the SAME backend the row was originally queued against
        // (multi-grid Slice 1c) — never the currently-focused context's key.
        const std::string backendKey =
            sel.getColumn(7).isNull() ? std::string() : std::string(sel.getColumn(7).getText());
        const auto now = std::chrono::system_clock::now();
        const std::int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        SQLite::Statement ins(db, "INSERT INTO pending_field_edits (issue_key, field_id, fields_payload_json, "
                                  "original_rich_value, original_value, has_original_value, backend_key, attempts, "
                                  "last_error, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, 0, '', ?)");
        ins.bind(1, issueKey);
        ins.bind(2, fieldId);
        ins.bind(3, payload);
        if (richIsNull)
            ins.bind(4); // NULL
        else
            ins.bind(4, originalRichValue);
        if (valueIsNull)
            ins.bind(5); // NULL
        else
            ins.bind(5, originalValue);
        ins.bind(6, hasOriginalValue ? 1 : 0);
        ins.bind(7, backendKey);
        ins.bind(8, epoch);
        ins.exec();
        SQLite::Statement delDead(db, "DELETE FROM pending_field_edits_dead WHERE dead_id = ?");
        delDead.bind(1, deadId);
        delDead.exec();
        transaction.commit();
        LOG_INFO("LocalCacheManager::RestoreDeadPendingFieldEdit original_id=%lld dead_id=%lld",
                 static_cast<long long>(originalPendingId), static_cast<long long>(deadId));
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RestoreDeadPendingFieldEdit failed original_id=%lld err=%s",
                  static_cast<long long>(originalPendingId), ex.what());
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
