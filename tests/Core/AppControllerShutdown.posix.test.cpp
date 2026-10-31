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

#include <atomic>
#include <chrono>
#include <memory>
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
