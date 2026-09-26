#include "LocalCacheManager.h"

#include "Logger.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <ctime>
#include <exception>
#include <string>
#include <utility>
#include <vector>

// LocalCacheManager's ILookupCache half (Quality Pillar 6): persisted read-side lookups such as the
// workflow remembered from earlier online use. Additive table; an older cache file gains it on open.
// Every method uses its own local SQLite::Statement, so none touches the cached-statement slots that
// stmtMutex_ guards (same shape as EnqueuePendingFieldEdit); the connection is OPEN_FULLMUTEX.

void LocalCacheManager::InitLookupCacheSchema_() {
    db.exec("CREATE TABLE IF NOT EXISTS lookup_cache ("
            "backend_key TEXT NOT NULL, kind TEXT NOT NULL, cache_key TEXT NOT NULL, "
            "payload_json TEXT NOT NULL, schema_version INTEGER NOT NULL DEFAULT 1, "
            "updated_at INTEGER NOT NULL, PRIMARY KEY (backend_key, kind, cache_key))");
}

bool LocalCacheManager::UpsertLookup(const std::string& backendKey, const std::string& kind,
                                     const std::string& cacheKey, const std::string& payloadJson) {
    try {
        const std::int64_t nowSec = std::time(nullptr);
        SQLite::Statement stmt(db, "INSERT OR REPLACE INTO lookup_cache (backend_key, kind, cache_key, "
                                   "payload_json, schema_version, updated_at) VALUES (?, ?, ?, ?, 1, ?)");
        stmt.bind(1, backendKey);
        stmt.bind(2, kind);
        stmt.bind(3, cacheKey);
        stmt.bind(4, payloadJson);
        stmt.bind(5, nowSec);
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::UpsertLookup failed: %s", ex.what());
        return false;
    }
}

bool LocalCacheManager::TryGetLookup(const std::string& backendKey, const std::string& kind,
                                     const std::string& cacheKey, LookupCacheRow& out) {
    try {
        SQLite::Statement stmt(db, "SELECT cache_key, payload_json, updated_at FROM lookup_cache "
                                   "WHERE backend_key = ? AND kind = ? AND cache_key = ?");
        stmt.bind(1, backendKey);
        stmt.bind(2, kind);
        stmt.bind(3, cacheKey);
        if (stmt.executeStep()) {
            out.CacheKey = stmt.getColumn(0).getText();
            out.PayloadJson = stmt.getColumn(1).getText();
            out.UpdatedAtEpochSec = stmt.getColumn(2).getInt64();
            return true;
        }
        return false;
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::TryGetLookup failed: %s", ex.what());
        return false;
    }
}

std::vector<LookupCacheRow> LocalCacheManager::LoadLookups(const std::string& backendKey, const std::string& kind) {
    std::vector<LookupCacheRow> result;
    try {
        SQLite::Statement stmt(db, "SELECT cache_key, payload_json, updated_at FROM lookup_cache "
                                   "WHERE backend_key = ? AND kind = ?");
        stmt.bind(1, backendKey);
        stmt.bind(2, kind);
        while (stmt.executeStep()) {
            LookupCacheRow row;
            row.CacheKey = stmt.getColumn(0).getText();
            row.PayloadJson = stmt.getColumn(1).getText();
            row.UpdatedAtEpochSec = stmt.getColumn(2).getInt64();
            result.push_back(std::move(row));
        }
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::LoadLookups failed: %s", ex.what());
    }
    return result;
}

bool LocalCacheManager::DeleteLookup(const std::string& backendKey, const std::string& kind,
                                     const std::string& cacheKey) {
    try {
        SQLite::Statement stmt(db, "DELETE FROM lookup_cache WHERE backend_key = ? AND kind = ? AND cache_key = ?");
        stmt.bind(1, backendKey);
        stmt.bind(2, kind);
        stmt.bind(3, cacheKey);
        stmt.exec();
        return true;
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::DeleteLookup failed: %s", ex.what());
        return false;
    }
}
