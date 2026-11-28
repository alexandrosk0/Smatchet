#pragma once
// LocalCacheManagerTestAccess — the one door a test uses to hold or read the cache's connection. It is a friend of
// LocalCacheManager and of its ConnGuard; the single definition lives here so every test TU sees the same type.

#include "LocalCacheManager.h"

struct LocalCacheManagerTestAccess {
    /// Holds the manager's connection guard for the duration of `fn(guard)`.
    template <typename Fn> static void WithGuard(LocalCacheManager& mgr, Fn&& fn) {
        const LocalCacheManager::ConnGuard guard(mgr, "TestHold");
        fn(guard);
    }
    static SQLite::Database& Conn(LocalCacheManager& mgr, const LocalCacheManager::ConnGuard& guard) {
        return mgr.conn(guard);
    }
    /// One integer result of `sql` on the manager's own connection (PRAGMAs, counts).
    static int QueryInt(LocalCacheManager& mgr, const char* sql) {
        int value = 0;
        WithGuard(mgr, [&](const LocalCacheManager::ConnGuard& guard) {
            SQLite::Statement q(mgr.conn(guard), sql);
            if (q.executeStep()) {
                value = q.getColumn(0).getInt();
            }
        });
        return value;
    }
    /// Runs the post-rebuild salvage from `quarantinedMain` (and its -wal, which may not exist) into `mgr`.
    static void Salvage(LocalCacheManager& mgr, const std::string& quarantinedMain, const std::string& quarantinedWal,
                        const std::string& reason) {
        WithGuard(mgr, [&](const LocalCacheManager::ConnGuard& guard) {
            mgr.SalvageFromQuarantine_(guard, quarantinedMain, quarantinedWal, reason);
        });
    }
};
