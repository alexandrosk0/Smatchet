#ifndef SMATCHET_TESTS_SQLITE_MEM_FIXTURE_H
#define SMATCHET_TESTS_SQLITE_MEM_FIXTURE_H

// SqliteMemFixture — RAII helper that opens a fresh LocalCacheManager against an in-memory
// SQLite database. Used by tests that exercise SaveTicket / pending-queue / dead-letter
// paths without writing a real DB file to disk.
//
// Why this works: SQLiteCpp accepts ":memory:" as a path; the resulting database lives in
// the process address space, is private to this connection, and is destroyed when the
// LocalCacheManager destructor runs. No file system, no test ordering risk, no cleanup.
//
// Header-only so test TUs can include without CMake glue beyond the existing
// `target_include_directories(... tests/support)`.

#include "LocalCacheManager.h"

#include <memory>
#include <string>

namespace smatchet_tests {

class SqliteMemFixture {
  public:
    SqliteMemFixture() : cache_(std::make_unique<LocalCacheManager>(":memory:")) {}

    /// Borrowed pointer — the fixture owns the LocalCacheManager. Test asserts must not
    /// outlive the fixture instance.
    LocalCacheManager* Get() { return cache_.get(); }
    LocalCacheManager& Ref() { return *cache_; }

    /// Drop and reopen — useful for tests that simulate "close the app, reopen, ensure the
    /// schema is still consistent". Note: an in-memory DB doesn't persist across this, so
    /// this is for testing that the init path itself is idempotent + side-effect-free.
    void Reopen() { cache_ = std::make_unique<LocalCacheManager>(":memory:"); }

  private:
    std::unique_ptr<LocalCacheManager> cache_;
};

} // namespace smatchet_tests

#endif
