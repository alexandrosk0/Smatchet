// LocalCacheManager's interruptible open (OpenOptions::Abort): an open that is waiting for a lock another connection
// holds gives up promptly once asked to, instead of waiting out the whole busy timeout, and the ordinary busy timeout
// is back when the constructor returns.

#include "LocalCacheManagerTestAccess.h"
#include "TempDbFile.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>

TEST_CASE("LocalCacheManager open: an abort ends a wait on a locked file promptly") {
    smatchet_tests::TempDbFile file("smatchet_open_abort_");
    {
        // A first connection creates the file in rollback-journal mode, then holds the exclusive lock.
        SQLite::Database holder(file.Path(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        holder.exec("CREATE TABLE held (a INTEGER)");
        holder.exec("BEGIN EXCLUSIVE");

        std::atomic<bool> abort{false};
        std::atomic<bool> threw{false};
        std::atomic<bool> finished{false};
        std::thread opener([&]() {
            try {
                LocalCacheManager::OpenOptions options;
                options.Abort = &abort;
                LocalCacheManager mgr(file.Path(), options);
            } catch (const std::exception&) {
                threw.store(true);
            }
            finished.store(true);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_FALSE(finished.load()); // still waiting on the lock
        const auto raised = std::chrono::steady_clock::now();
        abort.store(true);
        opener.join();
        const long long reactionMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - raised).count();

        CHECK(threw.load());
        CHECK(reactionMs < 2000); // the busy timeout alone would be 5000 ms per attempt
        holder.exec("ROLLBACK");
    }
}

TEST_CASE("LocalCacheManager open: with Abort unset the open succeeds and the busy timeout is restored") {
    smatchet_tests::TempDbFile file("smatchet_open_options_");
    std::atomic<bool> abort{false};
    LocalCacheManager::OpenOptions options;
    options.Abort = &abort;
    LocalCacheManager mgr(file.Path(), options);
    CHECK(LocalCacheManagerTestAccess::QueryInt(mgr, "PRAGMA busy_timeout") == 5000);
    CHECK(mgr.EnqueuePendingCreate("Jira@test#1", "{}") > 0);
}

TEST_CASE("LocalCacheManager open: the one-argument constructor keeps its behaviour") {
    smatchet_tests::TempDbFile file("smatchet_open_plain_");
    LocalCacheManager mgr(file.Path());
    CHECK(LocalCacheManagerTestAccess::QueryInt(mgr, "PRAGMA busy_timeout") == 5000);
}
