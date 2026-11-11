#include "LocalCacheManager.h"
#include "Logger.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

// The history of issues that left a view (docs/plans/left-view-history-grid.md). Additive table; an older cache
// file gains it on open. Worker threads only. A listing and the rows it produced commit in one transaction, so a
// crash can never move the listing past a departure it did not record.

namespace {

constexpr const char* kViewListingKind = "view_listing";

void TrimLeftViewRows(SQLite::Database& db, int maxRows) {
    SQLite::Statement trim(db, "DELETE FROM left_view_history WHERE id NOT IN "
                               "(SELECT id FROM left_view_history ORDER BY left_at DESC, id DESC LIMIT ?)");
    trim.bind(1, maxRows);
    trim.exec();
}

} // namespace

void LocalCacheManager::InitLeftViewHistorySchema_() {
    db.exec("CREATE TABLE IF NOT EXISTS left_view_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "backend_key TEXT NOT NULL, pane_id TEXT NOT NULL, view_id TEXT NOT NULL, view_name TEXT NOT NULL, "
            "query TEXT NOT NULL, issue_key TEXT NOT NULL, summary TEXT NOT NULL, kind INTEGER NOT NULL, "
            "reason_kind INTEGER NOT NULL, reason TEXT NOT NULL, reason_detail_json TEXT NOT NULL, "
            "author TEXT NOT NULL, browse_url TEXT NOT NULL, last_known_json TEXT NOT NULL, "
            "left_at INTEGER NOT NULL, listed_at INTEGER NOT NULL)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_left_view_history_left_at ON left_view_history(left_at DESC, id DESC)");
}

bool LocalCacheManager::CommitLeftViewDepartures(std::vector<smatchet::LeftViewRecord>& rows, int maxRows,
                                                 const std::string& backendKey, const std::string& viewId,
                                                 const std::string& listingJson, std::int64_t listingSavedAtUnixSec) {
    try {
        // The connection is shared: another thread's BEGIN or COMMIT must not land inside this transaction.
        std::lock_guard<std::mutex> lock(stmtMutex_);
        SQLite::Transaction transaction(db);
        SQLite::Statement insert(db, "INSERT INTO left_view_history (backend_key, pane_id, view_id, view_name, query, "
                                     "issue_key, summary, kind, reason_kind, reason, reason_detail_json, author, "
                                     "browse_url, last_known_json, left_at, listed_at) "
                                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id");
        for (smatchet::LeftViewRecord& row : rows) {
            insert.reset();
            insert.clearBindings();
            insert.bind(1, row.backendKey);
            insert.bind(2, row.paneId);
            insert.bind(3, row.viewId);
            insert.bind(4, row.viewName);
            insert.bind(5, row.query);
            insert.bind(6, row.issueKey);
            insert.bind(7, row.summary);
            insert.bind(8, static_cast<int>(row.kind));
            insert.bind(9, static_cast<int>(row.reasonKind));
            insert.bind(10, row.reason);
            insert.bind(11, row.reasonDetailJson);
            insert.bind(12, row.author);
            insert.bind(13, row.browseUrl);
            insert.bind(14, row.lastKnownJson);
            insert.bind(15, static_cast<std::int64_t>(row.leftAtUnixSec));
            insert.bind(16, static_cast<std::int64_t>(row.listedAtUnixSec));
            // RETURNING, not last_insert_rowid: a lookup upsert on another thread can move that between statements.
            if (insert.executeStep()) {
                row.id = insert.getColumn(0).getInt64();
            }
            insert.reset(); // a statement still stepping would block the COMMIT
        }
        SQLite::Statement listing(db, "INSERT OR REPLACE INTO lookup_cache (backend_key, kind, cache_key, "
                                      "payload_json, schema_version, updated_at) VALUES (?, ?, ?, ?, 1, ?)");
        listing.bind(1, backendKey);
        listing.bind(2, kViewListingKind);
        listing.bind(3, viewId);
        listing.bind(4, listingJson);
        listing.bind(5, static_cast<std::int64_t>(listingSavedAtUnixSec));
        listing.exec();
        if (!rows.empty()) {
            TrimLeftViewRows(db, maxRows);
        }
        transaction.commit();
        return true;
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::CommitLeftViewDepartures failed rows=%zu: %s", rows.size(), ex.what());
        return false;
    }
}

std::vector<smatchet::LeftViewRecord> LocalCacheManager::LoadLeftViewRows(int maxRows) {
    std::vector<smatchet::LeftViewRecord> out;
    try {
        SQLite::Statement stmt(db, "SELECT id, backend_key, pane_id, view_id, view_name, query, issue_key, summary, "
                                   "kind, reason_kind, reason, reason_detail_json, author, browse_url, "
                                   "last_known_json, left_at, listed_at FROM left_view_history "
                                   "ORDER BY left_at DESC, id DESC LIMIT ?");
        stmt.bind(1, maxRows);
        while (stmt.executeStep()) {
            smatchet::LeftViewRecord row;
            row.id = stmt.getColumn(0).getInt64();
            row.backendKey = stmt.getColumn(1).getText();
            row.paneId = stmt.getColumn(2).getText();
            row.viewId = stmt.getColumn(3).getText();
            row.viewName = stmt.getColumn(4).getText();
            row.query = stmt.getColumn(5).getText();
            row.issueKey = stmt.getColumn(6).getText();
            row.summary = stmt.getColumn(7).getText();
            row.kind = static_cast<smatchet::LeftViewKind>(stmt.getColumn(8).getInt());
            row.reasonKind = static_cast<smatchet::LeftViewReasonKind>(stmt.getColumn(9).getInt());
            row.reason = stmt.getColumn(10).getText();
            row.reasonDetailJson = stmt.getColumn(11).getText();
            row.author = stmt.getColumn(12).getText();
            row.browseUrl = stmt.getColumn(13).getText();
            row.lastKnownJson = stmt.getColumn(14).getText();
            row.leftAtUnixSec = stmt.getColumn(15).getInt64();
            row.listedAtUnixSec = stmt.getColumn(16).getInt64();
            out.push_back(row);
        }
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::LoadLeftViewRows failed: %s", ex.what());
    }
    return out;
}

bool LocalCacheManager::UpdateLeftViewReason(const smatchet::LeftViewRecord& row) {
    try {
        SQLite::Statement stmt(db, "UPDATE left_view_history SET kind = ?, reason_kind = ?, reason = ?, "
                                   "reason_detail_json = ?, author = ?, browse_url = ?, summary = ? WHERE id = ?");
        stmt.bind(1, static_cast<int>(row.kind));
        stmt.bind(2, static_cast<int>(row.reasonKind));
        stmt.bind(3, row.reason);
        stmt.bind(4, row.reasonDetailJson);
        stmt.bind(5, row.author);
        stmt.bind(6, row.browseUrl);
        stmt.bind(7, row.summary);
        stmt.bind(8, static_cast<std::int64_t>(row.id));
        return stmt.exec() > 0;
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::UpdateLeftViewReason failed: %s", ex.what());
        return false;
    }
}

bool LocalCacheManager::DeleteLeftViewRow(std::int64_t id) {
    try {
        SQLite::Statement stmt(db, "DELETE FROM left_view_history WHERE id = ?");
        stmt.bind(1, static_cast<std::int64_t>(id));
        return stmt.exec() > 0;
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::DeleteLeftViewRow failed: %s", ex.what());
        return false;
    }
}

bool LocalCacheManager::ClearLeftViewRows() {
    try {
        db.exec("DELETE FROM left_view_history");
        return true;
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager::ClearLeftViewRows failed: %s", ex.what());
        return false;
    }
}
