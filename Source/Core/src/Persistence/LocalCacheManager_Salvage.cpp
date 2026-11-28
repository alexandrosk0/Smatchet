// LocalCacheManager — best-effort salvage of the offline queues from a corrupt cache file that was just moved aside.
//
// A corrupt file is quarantined and replaced by an empty cache (RebuildFreshAfterCorruption). The queues in it hold the
// user's unsent writes, so before the quarantined copy is forgotten the queue tables are read back into the new file.
// This is best effort and says so: a table that cannot be read, or whose shape does not match, is named in the recovery
// report instead of being silently dropped, and the report is stored so a later launch can still tell the user.
//
// Runs inside the constructor's rebuild, so it holds the connection guard already and never opens a RunTxn_.

#include "Logger.h"
#include "LocalCacheManagerInternal.h"
#include "Persistence/LocalCacheStatusPure.h"
#include "PendingActionTypes.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <ghc/filesystem.hpp>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = ghc::filesystem;

/// Every table that carries the user's unsent work, plus `cache_meta` (its flags describe those same rows). A new queue
/// table must be added here, or its rows are lost on a rebuild.
const char* const kSalvageTables[] = {
    "pending_creates", "pending_creates_dead", "pending_field_edits", "pending_field_edits_dead",
    "pending_actions", "pending_actions_dead", "cache_meta"};

/// Written by the salvage itself, after the copy, so the new record always wins over one copied from the old file.
constexpr const char* kRecoveryMetaKey = "last_corruption_recovery_v1";

struct ColumnInfo {
    std::string Name;
    bool NotNull = false;
    bool HasDefault = false;
    bool IsPrimaryKey = false;
};

/// Identifiers are spliced into SQL, so only plain ones are ever used.
bool IsPlainIdentifier(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c) && c != '_') {
            return false;
        }
    }
    return true;
}

std::string Quoted(const std::string& identifier) { return "\"" + identifier + "\""; }

std::vector<ColumnInfo> ReadColumns(SQLite::Database& db, const char* schema, const std::string& table) {
    std::vector<ColumnInfo> columns;
    SQLite::Statement info(db, std::string("PRAGMA ") + schema + ".table_info(" + Quoted(table) + ")");
    while (info.executeStep()) {
        ColumnInfo column;
        column.Name = info.getColumn(1).getString();
        column.NotNull = info.getColumn(3).getInt() != 0;
        column.HasDefault = !info.getColumn(4).isNull();
        column.IsPrimaryKey = info.getColumn(5).getInt() != 0;
        columns.push_back(column);
    }
    return columns;
}

bool ContainsColumn(const std::vector<ColumnInfo>& columns, const std::string& name) {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].Name == name) {
            return true;
        }
    }
    return false;
}

bool SalvageHasTable(SQLite::Database& db, const std::string& table) {
    SQLite::Statement q(db, "SELECT 1 FROM salvage.sqlite_master WHERE type = 'table' AND name = ? AND rootpage > 0");
    q.bind(1, table);
    return q.executeStep();
}

/// The columns both files have and the new file can fill in. Empty when the shapes do not match: a column the new file
/// requires but the old one lacks would make every row fail.
bool SharedColumns(SQLite::Database& db, const std::string& table, std::vector<std::string>& out) {
    const std::vector<ColumnInfo> mainColumns = ReadColumns(db, "main", table);
    const std::vector<ColumnInfo> oldColumns = ReadColumns(db, "salvage", table);
    for (std::size_t i = 0; i < mainColumns.size(); ++i) {
        const ColumnInfo& column = mainColumns[i];
        if (!IsPlainIdentifier(column.Name)) {
            return false;
        }
        if (ContainsColumn(oldColumns, column.Name)) {
            out.push_back(column.Name);
        } else if (column.NotNull && !column.HasDefault && !column.IsPrimaryKey) {
            return false;
        }
    }
    return !out.empty();
}

std::string JoinQuoted(const std::vector<std::string>& names) {
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
        joined += (i == 0 ? "" : ", ") + Quoted(names[i]);
    }
    return joined;
}

/// Row-by-row fallback when the bulk copy failed: copies every row that can be read, and reports whether the whole
/// table was read. A page that cannot be read ends the walk.
bool CopyRowsOneByOne(SQLite::Database& db, const std::string& table, const std::vector<std::string>& columns,
                      std::int64_t& copied) {
    std::string placeholders;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        placeholders += i == 0 ? "?" : ", ?";
    }
    SQLite::Statement insert(db, "INSERT OR IGNORE INTO main." + Quoted(table) + " (" + JoinQuoted(columns) +
                                     ") VALUES (" + placeholders + ")");
    bool complete = true;
    try {
        SQLite::Statement select(db, "SELECT " + JoinQuoted(columns) + " FROM salvage." + Quoted(table));
        while (select.executeStep()) {
            insert.reset();
            insert.clearBindings();
            for (std::size_t i = 0; i < columns.size(); ++i) {
                const SQLite::Column column = select.getColumn(static_cast<int>(i));
                const int index = static_cast<int>(i) + 1;
                if (column.isNull()) {
                    insert.bind(index);
                } else if (column.isInteger()) {
                    insert.bind(index, static_cast<std::int64_t>(column.getInt64()));
                } else if (column.isFloat()) {
                    insert.bind(index, column.getDouble());
                } else {
                    insert.bind(index, column.getString());
                }
            }
            try {
                copied += insert.exec();
            } catch (const SQLite::Exception&) {
                complete = false; // this one row would not go in; the rest still can
            }
        }
    } catch (const SQLite::Exception&) {
        complete = false; // a page of the old file could not be read: everything after it is lost
    }
    return complete;
}

/// Copies one table. False when any of it could not be carried over.
bool CopyTable(SQLite::Database& db, const std::string& table, std::int64_t& copied) {
    try {
        if (!SalvageHasTable(db, table)) {
            return true; // an older file without this table has nothing to carry over
        }
        std::vector<std::string> columns;
        if (!SharedColumns(db, table, columns)) {
            return false;
        }
        db.exec("SAVEPOINT salvage_copy");
        try {
            const int rows = db.exec("INSERT OR IGNORE INTO main." + Quoted(table) + " (" + JoinQuoted(columns) +
                                     ") SELECT " + JoinQuoted(columns) + " FROM salvage." + Quoted(table));
            db.exec("RELEASE salvage_copy");
            copied += rows;
            return true;
        } catch (const SQLite::Exception&) {
            db.exec("ROLLBACK TO salvage_copy");
            db.exec("RELEASE salvage_copy");
        }
        return CopyRowsOneByOne(db, table, columns, copied);
    } catch (const SQLite::Exception& ex) {
        LOG_WARN("LocalCacheManager: salvage of table %s failed: %s", table.c_str(), ex.what());
        return false;
    }
}

void RemoveQuietly(const std::string& path) {
    std::error_code ec;
    fs::remove(fs::u8path(path), ec);
}

bool CopyQuietly(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::copy_file(fs::u8path(from), fs::u8path(to), fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool PathExistsQuietly(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec) && !ec;
}

} // namespace

void LocalCacheManager::SalvageFromQuarantine_(const ConnGuard& guard, const std::string& quarantinedMain,
                                               const std::string& quarantinedWal, const std::string& reason) noexcept {
    recovery_ = smatchet::persistence::LocalCacheRecoveryReport();
    recovery_.Rebuilt = true;
    recovery_.WhenSec = static_cast<std::int64_t>(std::time(nullptr));
    recovery_.QuarantinedPath = quarantinedMain;
    recovery_.Reason = reason;
    recovery_.SalvageAttempted = true;
    recovery_.FromThisLaunch = true;

    const std::string salvagePath = quarantinedMain + ".salvage";
    std::int64_t copied = 0;
    bool attached = false;
    SQLite::Database* db = nullptr;
    try {
        db = &conn(guard);
        // Work on a copy: opening the quarantined file in place could write a journal beside it, and the copy can be
        // opened without its -wal when the WAL is what is damaged.
        if (!CopyQuietly(quarantinedMain, salvagePath)) {
            throw std::runtime_error("the damaged file could not be copied for salvage");
        }
        const bool hadWal = PathExistsQuietly(quarantinedWal) && CopyQuietly(quarantinedWal, salvagePath + "-wal");
        for (int attempt = 0; attempt < 2 && !attached; ++attempt) {
            try {
                SQLite::Statement attach(*db, "ATTACH DATABASE ? AS salvage");
                attach.bind(1, salvagePath);
                attach.exec();
                // Reading proves the file opens at all (an ATTACH of a non-database succeeds until the first read).
                SQLite::Statement probe(*db, "SELECT COUNT(*) FROM salvage.sqlite_master");
                probe.executeStep();
                attached = true;
            } catch (const SQLite::Exception& ex) {
                LOG_WARN("LocalCacheManager: salvage attach failed (%s)%s", ex.what(),
                         attempt == 0 && hadWal ? "; retrying without the WAL" : "");
                try {
                    db->exec("DETACH DATABASE salvage");
                } catch (const SQLite::Exception&) {
                    // not attached: nothing to detach
                }
                if (attempt == 0 && hadWal) {
                    RemoveQuietly(salvagePath + "-wal");
                } else {
                    break;
                }
            }
        }
        if (!attached) {
            for (std::size_t i = 0; i < sizeof(kSalvageTables) / sizeof(kSalvageTables[0]); ++i) {
                recovery_.IncompleteTables.push_back(kSalvageTables[i]);
            }
        } else {
            for (std::size_t i = 0; i < sizeof(kSalvageTables) / sizeof(kSalvageTables[0]); ++i) {
                if (!CopyTable(*db, kSalvageTables[i], copied)) {
                    recovery_.IncompleteTables.push_back(kSalvageTables[i]);
                }
            }
            try {
                db->exec("DETACH DATABASE salvage");
            } catch (const SQLite::Exception& ex) {
                LOG_WARN("LocalCacheManager: salvage detach failed: %s", ex.what());
            }
            // A request that was `sending` when the file broke may or may not have reached the tracker, exactly as one
            // found `sending` on an ordinary open: it is checked before it is ever resent.
            SQLite::Statement ambiguous(*db, "UPDATE pending_actions SET state = ? WHERE state = ?");
            ambiguous.bind(1, PendingActionState::kAmbiguous);
            ambiguous.bind(2, PendingActionState::kSending);
            ambiguous.exec();
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("LocalCacheManager: salvage from '%s' failed: %s", quarantinedMain.c_str(), ex.what());
        if (recovery_.IncompleteTables.empty()) {
            for (std::size_t i = 0; i < sizeof(kSalvageTables) / sizeof(kSalvageTables[0]); ++i) {
                recovery_.IncompleteTables.push_back(kSalvageTables[i]);
            }
        }
    }
    RemoveQuietly(salvagePath);
    RemoveQuietly(salvagePath + "-wal");
    RemoveQuietly(salvagePath + "-shm");
    RemoveQuietly(salvagePath + "-journal");

    recovery_.SalvagedRows = copied;
    LOG_WARN(
        "LocalCacheManager: rebuilt the cache after damage; salvaged %lld row(s), %zu table(s) incomplete; the damaged "
        "copy is kept at '%s'",
        static_cast<long long>(copied), recovery_.IncompleteTables.size(), quarantinedMain.c_str());
    try {
        if (db != nullptr) {
            SQLite::Statement record(*db, "INSERT OR REPLACE INTO cache_meta (key, value) VALUES (?, ?)");
            record.bind(1, kRecoveryMetaKey);
            record.bind(2, smatchet::persistence::EncodeRecoveryReport(recovery_));
            record.exec();
        }
    } catch (const std::exception& ex) {
        LOG_WARN("LocalCacheManager: the recovery record could not be stored: %s", ex.what());
    }
}

bool LocalCacheManager::ReadLastRecoveryReport(smatchet::persistence::LocalCacheRecoveryReport& out) {
    const ConnGuard guard(*this, "ReadLastRecoveryReport");
    SQLite::Statement q(conn(guard), "SELECT value FROM cache_meta WHERE key = ?");
    q.bind(1, kRecoveryMetaKey);
    if (!q.executeStep()) {
        return false;
    }
    return smatchet::persistence::DecodeRecoveryReport(q.getColumn(0).getString(), out);
}
