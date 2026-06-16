#pragma once
// TempDbFile — RAII unique on-disk SQLite path for tests that genuinely need a FILE-backed DB:
// two connections seeing the same bytes (a raw writer then a LocalCacheManager), a reopen across
// ctor cycles, or a deliberately corrupt/truncated file. ":memory:" is per-connection and so can
// reach none of these (SqliteMemFixture covers the in-memory path). The full SQLite file family
// (<path>, -wal, -shm, -journal) is removed on destruction.
//
// Header-only, C++14, no deps beyond the stdlib. `prefix` only flavours the temp filename for
// readability in a stray-file listing — nothing asserts on the path string.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace smatchet_tests {

class TempDbFile {
  public:
    explicit TempDbFile(const char* prefix = "smatchet_test_db_") {
        static std::atomic<int> counter{0};
        const char* base = std::getenv("TEMP");
        if (!base) {
            base = std::getenv("TMP");
        }
        if (!base) {
            base = std::getenv("TMPDIR");
        }
        const long long stamp = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::string(base ? base : ".") + "/" + prefix + std::to_string(stamp) + "_" +
                std::to_string(counter.fetch_add(1)) + ".db";
    }
    ~TempDbFile() {
        std::remove(path_.c_str());
        std::remove((path_ + "-wal").c_str());
        std::remove((path_ + "-shm").c_str());
        std::remove((path_ + "-journal").c_str());
    }

    TempDbFile(const TempDbFile&) = delete;
    TempDbFile& operator=(const TempDbFile&) = delete;

    const std::string& Path() const { return path_; }

  private:
    std::string path_;
};

} // namespace smatchet_tests
