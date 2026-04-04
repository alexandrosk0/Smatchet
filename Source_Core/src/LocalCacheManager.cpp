#include "LocalCacheManager.h"

LocalCacheManager::LocalCacheManager(const std::string& dbPath) 
    : db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) 
{
    // Create table if it doesn't exist
    db.exec("CREATE TABLE IF NOT EXISTS tickets (id TEXT PRIMARY KEY)");
    db.exec("CREATE TABLE IF NOT EXISTS ticket_field_values ("
            "ticket_id TEXT NOT NULL, "
            "field_key TEXT NOT NULL, "
            "field_value TEXT, "
            "PRIMARY KEY(ticket_id, field_key))");
}

void LocalCacheManager::SaveTicket(const CachedTicket& ticket) {
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
}

void LocalCacheManager::DeleteTicket(const std::string& ticketId) {
    SQLite::Transaction transaction(db);
    SQLite::Statement deleteFields(db, "DELETE FROM ticket_field_values WHERE ticket_id = ?");
    deleteFields.bind(1, ticketId);
    deleteFields.exec();
    SQLite::Statement deleteTicket(db, "DELETE FROM tickets WHERE id = ?");
    deleteTicket.bind(1, ticketId);
    deleteTicket.exec();
    transaction.commit();
}

std::vector<CachedTicket> LocalCacheManager::GetAllTickets() {
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

    SQLite::Statement fieldQuery(db, "SELECT ticket_id, field_key, field_value FROM ticket_field_values");
    while (fieldQuery.executeStep()) {
        const std::string ticketId = fieldQuery.getColumn(0).getText();
        const auto it = indexById.find(ticketId);
        if (it == indexById.end()) {
            continue;
        }

        const std::string fieldKey = fieldQuery.getColumn(1).getText();
        const std::string fieldValue = fieldQuery.getColumn(2).isNull()
            ? std::string()
            : std::string(fieldQuery.getColumn(2).getText());
        results[it->second].fieldValues[fieldKey] = fieldValue;
    }
    return results;
}