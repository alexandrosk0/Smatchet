// LocalCacheManager's one-time move of cached rows from the legacy tracker-kind keys ("Jira", "Plane", ...)
// to the site keys that namespace the cache by tracker site and account (#2268).

#include "LocalCacheManager.h"

#include "Logger.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kCacheIdentityRekeyFlag = "cache_identity_rekey_v1";

// Cache tables: a primary-key collision keeps the row already under the site key (UPDATE OR IGNORE), and the
// legacy duplicate is then dropped — it is cached data the next sync refetches.
const char* const kCacheTables[] = {"tickets_v2", "ticket_field_values_v2", "ticket_field_rich_values_v2",
                                    "lookup_cache"};
// Queue tables: rows are keyed by an auto-increment id, so a plain UPDATE never collides. Queued writes are
// never dropped here.
const char* const kQueueTables[] = {"pending_field_edits",  "pending_field_edits_dead", "pending_creates",
                                    "pending_creates_dead", "pending_actions",          "pending_actions_dead"};

std::size_t RekeyRows(SQLite::Database& db, const std::string& sql, const std::string& to, const std::string& from) {
    SQLite::Statement st(db, sql);
    st.bind(1, to);
    st.bind(2, from);
    st.exec();
    const int changed = db.getChanges();
    return changed > 0 ? static_cast<std::size_t>(changed) : 0u;
}

void DropRows(SQLite::Database& db, const char* table, const std::string& from) {
    SQLite::Statement st(db, std::string("DELETE FROM ") + table + " WHERE backend_key = ?");
    st.bind(1, from);
    st.exec();
}

} // namespace

std::size_t
LocalCacheManager::RunOneTimeCacheIdentityRekey(const std::vector<std::pair<std::string, std::string>>& fromTo) {
    try {
        SQLite::Transaction transaction(db);
        SQLite::Statement probe(db, "SELECT 1 FROM cache_meta WHERE key = ? LIMIT 1");
        probe.bind(1, kCacheIdentityRekeyFlag);
        if (probe.executeStep()) {
            transaction.commit();
            return 0;
        }
        std::size_t moved = 0;
        for (const std::pair<std::string, std::string>& rekey : fromTo) {
            const std::string& from = rekey.first;
            const std::string& to = rekey.second;
            if (from.empty() || to.empty() || from == to) {
                continue; // never re-key into or out of the empty namespace
            }
            for (const char* table : kCacheTables) {
                moved += RekeyRows(
                    db, std::string("UPDATE OR IGNORE ") + table + " SET backend_key = ? WHERE backend_key = ?", to,
                    from);
                DropRows(db, table, from);
            }
            for (const char* table : kQueueTables) {
                moved += RekeyRows(db, std::string("UPDATE ") + table + " SET backend_key = ? WHERE backend_key = ?",
                                   to, from);
            }
        }
        SQLite::Statement flag(db, "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, '1')");
        flag.bind(1, kCacheIdentityRekeyFlag);
        flag.exec();
        transaction.commit();
        if (moved > 0) {
            LOG_INFO("LocalCacheManager::RunOneTimeCacheIdentityRekey moved rows=%zu across %zu key(s)", moved,
                     fromTo.size());
        }
        return moved;
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager::RunOneTimeCacheIdentityRekey failed err=%s", ex.what());
        throw;
    }
}
