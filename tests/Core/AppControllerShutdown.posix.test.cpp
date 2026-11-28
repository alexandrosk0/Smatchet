// ~AppController joins every background task, including one a background worker launches while
// shutdown is starting (#2308). LaunchBackgroundTask used to check the shutdown flag, create the
// thread, and only then take the worker-list lock to register it; a launch that passed the check
// just before the flag flipped could register its thread after JoinBackgroundTasks had taken its
// snapshot, so nothing joined it and the joinable std::thread's destructor called std::terminate.
// The check now happens under the same lock hold as the registration.
//
// The race is timing-dependent, so this drives it many times: several self-relaunching task chains
// keep a launch in flight at every moment while the controller is destroyed. A lost worker aborts
// the process (std::terminate); a task outliving the controller shows up as a run counted after the
// destructor returned. Linked into SmatchetCommandsTests with the real (headless) AppController.

#include <doctest/doctest.h>

#include "../support/OfflineQueueTestEnv.h"

#include "AppController.h"
#include "CachedTicketTypes.h"
#include "ConfigManager.h"
#include "LocalCacheManager.h"
#include "LookupPayloadsPure.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

// Runs once, then launches its successor unless the controller is shutting down, so one run of each
// chain is always either executing or inside LaunchBackgroundTask.
void LaunchChain(AppController& app, const std::shared_ptr<std::atomic<long>>& runs) {
    app.LaunchBackgroundTask([&app, runs]() {
        runs->fetch_add(1, std::memory_order_relaxed);
        if (!app.IsShuttingDown()) {
            LaunchChain(app, runs);
        }
    });
}

} // namespace

TEST_CASE("~AppController joins tasks that background workers launch while shutdown begins (#2308)") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    for (int round = 0; round < 40; ++round) {
        const auto runs = std::make_shared<std::atomic<long>>(0);
        {
            const std::unique_ptr<AppController> app = std::make_unique<AppController>();
            for (int chain = 0; chain < 4; ++chain) {
                LaunchChain(*app, runs);
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (runs->load(std::memory_order_relaxed) < 16 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            REQUIRE(runs->load(std::memory_order_relaxed) >= 16);
        } // ~AppController: shutdown flag, then JoinBackgroundTasks
        const long atDestruction = runs->load(std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(runs->load(std::memory_order_relaxed) == atDestruction);
    }
}

// A save UpdateTicket queued just before quit must reach the file: ~AppController flushes the ticket-save queue
// before it flags shutdown. Every one of the quick-fire saves is present when the file is reopened.
TEST_CASE("~AppController leaves every queued UpdateTicket save in the cache file") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    const std::string path = ConfigManager::GetUserDataDirectory() + "shutdown_saves_test.sqlite";
    const auto removeFiles = [&path]() {
        for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::remove((path + suffix).c_str());
        }
    };
    removeFiles();
    constexpr int kSaves = 60;
    std::string backendKey;
    {
        AppController app;
        REQUIRE(app.OpenLocalCacheFileForTest(path));
        backendKey = app.FocusedCacheBackendKey();
        for (int i = 0; i < kSaves; ++i) {
            CachedTicket ticket;
            ticket.id = "SHUT-" + std::to_string(i);
            ticket.fieldValues["summary"] = "saved before quit " + std::to_string(i);
            app.UpdateTicket(ticket);
        }
    } // ~AppController: flush, shutdown flag, join, tail flush
    {
        LocalCacheManager reopened(path);
        for (int i = 0; i < kSaves; ++i) {
            CachedTicket row;
            const std::string id = "SHUT-" + std::to_string(i);
            REQUIRE(reopened.TryGetTicket(backendKey, id, row));
            CHECK(row.GetFieldValue("summary") == "saved before quit " + std::to_string(i));
        }
    }
    removeFiles();
}

// A save posted AFTER shutdown was flagged loses its drain (LaunchBackgroundTask refuses once the flag is set) and
// stays queued. The tail flush at the end of ~AppController runs it, so the row still reaches the file.
TEST_CASE("~AppController's tail flush saves an UpdateTicket posted after the shutdown flag") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    const std::string path = ConfigManager::GetUserDataDirectory() + "shutdown_tail_test.sqlite";
    const auto removeFiles = [&path]() {
        for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::remove((path + suffix).c_str());
        }
    };
    removeFiles();
    std::string backendKey;
    const auto started = std::make_shared<std::atomic<bool>>(false);
    {
        AppController app;
        REQUIRE(app.OpenLocalCacheFileForTest(path));
        backendKey = app.FocusedCacheBackendKey();
        // The task is joined by ~AppController, so it can outlive the flush that precedes the flag: it waits for the
        // flag, then posts its save, whose drain launch is refused.
        app.LaunchBackgroundTask([&app, started]() {
            started->store(true);
            while (!app.IsShuttingDown()) {
                std::this_thread::yield();
            }
            CachedTicket ticket;
            ticket.id = "TAIL-1";
            ticket.fieldValues["summary"] = "posted after the flag";
            app.UpdateTicket(ticket);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!started->load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        REQUIRE(started->load());
    } // ~AppController: pre-flag flush (nothing queued), flag, join (the task posts), tail flush runs the save
    {
        LocalCacheManager reopened(path);
        CachedTicket row;
        REQUIRE(reopened.TryGetTicket(backendKey, "TAIL-1", row));
        CHECK(row.GetFieldValue("summary") == "posted after the flag");
    }
    removeFiles();
}

// The user roster is saved latest-wins on its own queue: however many rosters are posted before the first write runs,
// the row that reaches the file is the newest, and ~AppController flushes it before the pool is flagged.
TEST_CASE("~AppController leaves the newest user roster in the cache file and never an older one") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    const std::string path = ConfigManager::GetUserDataDirectory() + "shutdown_roster_test.sqlite";
    const auto removeFiles = [&path]() {
        for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::remove((path + suffix).c_str());
        }
    };
    removeFiles();
    {
        AppController app;
        REQUIRE(app.OpenLocalCacheFileForTest(path));
        for (int i = 0; i < 40; ++i) {
            app.SaveAvailableUsersForOfflineAsync("Jira", "roster-a-" + std::to_string(i));
            app.SaveAvailableUsersForOfflineAsync("Jira:other.example", "roster-b-" + std::to_string(i));
        }
        app.SaveAvailableUsersForOfflineAsync("Jira", ""); // an empty roster saves nothing
    } // ~AppController: the pre-flag flush runs the queue
    {
        LocalCacheManager reopened(path);
        LookupCacheRow row;
        REQUIRE(reopened.TryGetLookup("Jira", smatchet::lookup::kUsersKind, smatchet::lookup::kUsersKey, row));
        CHECK(row.PayloadJson == "roster-a-39");
        REQUIRE(reopened.TryGetLookup("Jira:other.example", smatchet::lookup::kUsersKind, smatchet::lookup::kUsersKey,
                                      row));
        CHECK(row.PayloadJson == "roster-b-39");
    }
    removeFiles();
}

TEST_CASE("AppController::FlushUsersRosterSaves waits for a queued roster save") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    const std::string path = ConfigManager::GetUserDataDirectory() + "flush_roster_test.sqlite";
    const auto removeFiles = [&path]() {
        for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
            std::remove((path + suffix).c_str());
        }
    };
    removeFiles();
    AppController app;
    REQUIRE(app.OpenLocalCacheFileForTest(path));
    app.SaveAvailableUsersForOfflineAsync("Jira", "roster-1");
    REQUIRE(app.FlushUsersRosterSaves(std::chrono::seconds(10)));
    {
        LocalCacheManager second(path);
        LookupCacheRow row;
        REQUIRE(second.TryGetLookup("Jira", smatchet::lookup::kUsersKind, smatchet::lookup::kUsersKey, row));
        CHECK(row.PayloadJson == "roster-1");
    }
    // A second roster replaces the first.
    app.SaveAvailableUsersForOfflineAsync("Jira", "roster-2");
    REQUIRE(app.FlushUsersRosterSaves(std::chrono::seconds(10)));
    LocalCacheManager third(path);
    LookupCacheRow row;
    REQUIRE(third.TryGetLookup("Jira", smatchet::lookup::kUsersKind, smatchet::lookup::kUsersKey, row));
    CHECK(row.PayloadJson == "roster-2");
    removeFiles();
}
