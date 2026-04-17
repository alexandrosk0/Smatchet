#include "LocalCacheManager.h"
#include "Logger.h"

#include <exception>

LocalCacheManager::LocalCacheManager(const std::string& dbPath)
    : db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX)
{
    LOG_INFO("LocalCacheManager: opening db path=%s", dbPath.c_str());
    // WAL improves crash safety and allows readers while a writer is active.
    // synchronous=NORMAL is the recommended pairing with WAL (fsync on checkpoint, not every commit).
    try {
        db.exec("PRAGMA journal_mode=WAL");
        db.exec("PRAGMA synchronous=NORMAL");
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager: failed to set WAL pragmas: %s", ex.what());
    }
    db.exec("CREATE TABLE IF NOT EXISTS tickets (id TEXT PRIMARY KEY)");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_values ("
            "ticket_id TEXT NOT NULL, "
            "field_key TEXT NOT NULL, "
            "field_value TEXT, "
            "PRIMARY KEY(ticket_id, field_key))");
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
            db,
            "INSERT OR REPLACE INTO ticket_field_values (ticket_id, field_key, field_value) VALUES (?, ?, ?)");
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
            const std::string fieldValue = fieldQuery.getColumn(2).isNull()
                ? std::string()
                : std::string(fieldQuery.getColumn(2).getText());
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