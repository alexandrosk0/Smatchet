#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
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

class LocalCacheManager {
public:
    LocalCacheManager(const std::string& dbPath);
    
    void SaveTicket(const CachedTicket& ticket);
    std::vector<CachedTicket> GetAllTickets();

private:
    SQLite::Database db;
};