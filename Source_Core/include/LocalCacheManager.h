#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>

struct CachedTicket {
    std::string id;
    std::unordered_map<std::string, std::string> fieldValues;

    std::string GetFieldValue(const std::string& key) const {
        const auto it = fieldValues.find(key);
        return (it != fieldValues.end()) ? it->second : std::string();
    }
};

/**
 * Offline-queued issue create. `Payload` is the JSON-serialized IssueDraft
 * (see IssueDraftHelpers::ToJson). `Attempts` is bumped on each replay to
 * enforce a retry cap, and `LastError` shows the most recent failure reason
 * (surfaced in the UI so the user can decide to retry / drop).
 */
struct PendingCreate {
    std::int64_t Id = 0;
    std::string Payload;
    int Attempts = 0;
    std::string LastError;
    std::int64_t CreatedAtEpochSec = 0;
};

class LocalCacheManager {
public:
    LocalCacheManager(const std::string& dbPath);
    
    void SaveTicket(const CachedTicket& ticket);
    void DeleteTicket(const std::string& ticketId);
    std::vector<CachedTicket> GetAllTickets();

    /** @return generated row id. */
    std::int64_t EnqueuePendingCreate(const std::string& payload);
    std::vector<PendingCreate> LoadPendingCreates();
    void UpdatePendingCreate(std::int64_t id, int attempts, const std::string& lastError);
    void DeletePendingCreate(std::int64_t id);

private:
    SQLite::Database db;
};