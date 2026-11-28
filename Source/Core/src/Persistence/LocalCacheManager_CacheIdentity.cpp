// LocalCacheManager's one-time move of cached rows from the legacy tracker-kind keys ("Jira", "Plane", ...)
// to the site keys that namespace the cache by tracker site and account (#2268).

#include "LocalCacheManager.h"
#include "LocalCacheManagerInternal.h"

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
                                    "ticket_sync_meta", "lookup_cache"};
// Queue tables: rows are keyed by an auto-increment id, so a plain UPDATE never collides. Queued writes are
// never dropped here.
const char* const kQueueTables[] = {"pending_field_edits",  "pending_field_edits_dead", "pending_creates",
                                    "pending_creates_dead", "pending_actions",          "pending_actions_dead",
                                    "left_view_history"};

std::size_t RekeyRows(const LocalCacheManager::ConnGuard& /*guard*/, SQLite::Database& db, const std::string& sql,
                      const std::string& to, const std::string& from) {
    SQLite::Statement st(db, sql);
    st.bind(1, to);
    st.bind(2, from);
    st.exec();
    const int changed = db.getChanges();
    return changed > 0 ? static_cast<std::size_t>(changed) : 0u;
}

void DropRows(const LocalCacheManager::ConnGuard& /*guard*/, SQLite::Database& db, const char* table,
              const std::string& from) {
    SQLite::Statement st(db, std::string("DELETE FROM ") + table + " WHERE backend_key = ?");
    st.bind(1, from);
    st.exec();
}

} // namespace

std::size_t
LocalCacheManager::RunOneTimeCacheIdentityRekey(const std::vector<std::pair<std::string, std::string>>& fromTo) {
    try {
        // One transaction under the connection guard; the body returns the count and the log waits for the commit
        // (a BUSY retry re-runs the body).
        const std::size_t moved =
            RunTxn_("RunOneTimeCacheIdentityRekey", [&](const ConnGuard& guard, SQLite::Database& db) -> std::size_t {
                if (hasCacheMetaFlag_(guard, kCacheIdentityRekeyFlag)) {
                    return 0;
                }
                std::size_t rows = 0;
                for (const std::pair<std::string, std::string>& rekey : fromTo) {
                    const std::string& from = rekey.first;
                    const std::string& to = rekey.second;
                    if (from.empty() || to.empty() || from == to) {
                        continue; // never re-key into or out of the empty namespace
                    }
                    for (const char* table : kCacheTables) {
                        rows += RekeyRows(guard, db,
                                          std::string("UPDATE OR IGNORE ") + table +
                                              " SET backend_key = ? WHERE backend_key = ?",
                                          to, from);
                        DropRows(guard, db, table, from);
                    }
                    for (const char* table : kQueueTables) {
                        rows += RekeyRows(guard, db,
                                          std::string("UPDATE ") + table + " SET backend_key = ? WHERE backend_key = ?",
                                          to, from);
                    }
                }
                setCacheMetaFlag_(guard, kCacheIdentityRekeyFlag); // same connection: part of this transaction
                return rows;
            });
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
