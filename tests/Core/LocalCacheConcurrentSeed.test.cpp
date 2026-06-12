// LocalCacheConcurrentSeed.test.cpp — multi-grid-tabs.md Slice 5 TSan pass.
//
// Exercises the Slice-5a concurrent seed-read pattern: a background task calls
// GetAllTickets while the UI thread concurrently writes via SaveTicket. Both
// paths share one LocalCacheManager instance (OPEN_FULLMUTEX serialises the
// sqlite3* handle; stmtMutex_ guards cached prepared statements). TSan
// instruments this to detect any C++ data races in the seed path.
//
// ImGui-free closure: LocalCacheManager.cpp + IssueDraft.cpp + Logger.cpp only.

#include "../support/SqliteMemFixture.h"

#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

using smatchet_tests::SqliteMemFixture;

namespace {

constexpr const char* kBk = "Jira";
constexpr int kIterations = 50;

CachedTicket MakeSeedTicket(const std::string& id) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = "seed " + id;
    t.fieldValues["status"] = "Open";
    return t;
}

} // namespace

TEST_CASE("LocalCacheConcurrentSeed: concurrent GetAllTickets + SaveTicket from two threads") {
    SqliteMemFixture fix;

    // Pre-populate so the seed-reader always sees at least one ticket.
    for (int i = 0; i < 5; ++i) {
        fix.Ref().SaveTicket(kBk, MakeSeedTicket("SEED-" + std::to_string(i)));
    }

    std::atomic<int> readCount(0);
    std::atomic<int> writeCount(0);

    // Thread 1 (seed-reader): simulates EnsurePaneLiveSyncStarted background task
    // calling GetAllTickets to seed the pane's ActiveTickets on restart.
    std::thread reader([&] {
        for (int i = 0; i < kIterations; ++i) {
            std::vector<CachedTicket> tickets = fix.Ref().GetAllTickets(kBk);
            (void)tickets;
            ++readCount;
        }
    });

    // Thread 2 (writer): simulates UI-thread SaveTicket calls (field-edit apply,
    // streaming-sync apply loop) concurrent with the seed read.
    std::thread writer([&] {
        for (int i = 0; i < kIterations; ++i) {
            fix.Ref().SaveTicket(kBk, MakeSeedTicket("LIVE-" + std::to_string(i)));
            ++writeCount;
        }
    });

    reader.join();
    writer.join();

    CHECK(readCount.load() == kIterations);
    CHECK(writeCount.load() == kIterations);

    // Both threads complete without crash and the final seed includes at least
    // the pre-seeded rows.
    std::vector<CachedTicket> final_ = fix.Ref().GetAllTickets(kBk);
    CHECK(final_.size() >= static_cast<std::size_t>(5));
}
