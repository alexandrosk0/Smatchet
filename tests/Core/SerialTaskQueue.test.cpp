#include <doctest/doctest.h>

#include "SerialTaskQueue.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using smatchet::SerialTaskQueue;

TEST_CASE("SerialTaskQueue runs tasks in post order and a post during a drain joins it") {
    std::vector<std::function<void()>> launched;
    SerialTaskQueue queue([&launched](std::function<void()> drain) { launched.push_back(std::move(drain)); });
    std::vector<int> ran;
    CHECK(queue.Post([&ran]() { ran.push_back(1); }).empty());
    CHECK(queue.Post([&ran]() { ran.push_back(2); }).empty()); // queued behind the pending drain
    REQUIRE(launched.size() == 1);
    // A task that posts: the running drain picks the new task up instead of starting a second one.
    CHECK(queue
              .Post([&]() {
                  ran.push_back(3);
                  CHECK(queue.Post([&ran]() { ran.push_back(4); }).empty());
              })
              .empty());
    launched[0]();
    CHECK(ran == std::vector<int>{1, 2, 3, 4});
    CHECK(launched.size() == 1);
    // Idle again: the next post starts a new drain.
    CHECK(queue.Post([&ran]() { ran.push_back(5); }).empty());
    REQUIRE(launched.size() == 2);
    launched[1]();
    CHECK(ran.back() == 5);
}

TEST_CASE("SerialTaskQueue keeps a task whose launch failed and runs it on the next post") {
    bool failLaunch = true;
    SerialTaskQueue queue([&failLaunch](std::function<void()> drain) {
        if (failLaunch) {
            throw std::runtime_error("no threads");
        }
        drain();
    });
    std::vector<int> ran;
    CHECK(queue.Post([&ran]() { ran.push_back(1); }) == "no threads");
    CHECK(ran.empty());
    failLaunch = false;
    CHECK(queue.Post([&ran]() { ran.push_back(2); }).empty());
    CHECK(ran == std::vector<int>{1, 2});
}

TEST_CASE("SerialTaskQueue releases its drain when a task throws") {
    std::vector<std::function<void()>> launched;
    SerialTaskQueue queue([&launched](std::function<void()> drain) { launched.push_back(std::move(drain)); });
    std::vector<int> ran;
    CHECK(queue.Post([]() { throw std::runtime_error("disk gone"); }).empty());
    CHECK(queue.Post([&ran]() { ran.push_back(1); }).empty());
    REQUIRE(launched.size() == 1);
    CHECK_THROWS_AS(launched[0](), std::runtime_error);
    CHECK(ran.empty()); // the throw ended that drain
    // The next post starts a fresh drain, which runs what was left first.
    CHECK(queue.Post([&ran]() { ran.push_back(2); }).empty());
    REQUIRE(launched.size() == 2);
    launched[1]();
    CHECK(ran == std::vector<int>{1, 2});
}

TEST_CASE("SerialTaskQueue never overlaps tasks and keeps each poster's order under contention") {
    std::mutex threadsMutex;
    std::vector<std::thread> workers;
    SerialTaskQueue queue([&](std::function<void()> drain) {
        std::lock_guard<std::mutex> lock(threadsMutex);
        workers.emplace_back(std::move(drain));
    });

    constexpr int kPosters = 8;
    constexpr int kPostsEach = 200;
    std::atomic<int> active{0};
    std::atomic<bool> overlapped{false};
    // Written only inside tasks, which never overlap; read after every worker is joined.
    std::vector<std::vector<int>> seen(kPosters);
    std::vector<std::thread> posters;
    for (int t = 0; t < kPosters; ++t) {
        posters.emplace_back([&, t]() {
            for (int i = 0; i < kPostsEach; ++i) {
                queue.Post([&, t, i]() {
                    if (active.fetch_add(1) != 0) {
                        overlapped.store(true);
                    }
                    seen[static_cast<std::size_t>(t)].push_back(i);
                    active.fetch_sub(1);
                });
            }
        });
    }
    for (std::thread& p : posters) {
        p.join();
    }
    for (;;) {
        std::vector<std::thread> batch;
        {
            std::lock_guard<std::mutex> lock(threadsMutex);
            batch.swap(workers);
        }
        if (batch.empty()) {
            break;
        }
        for (std::thread& w : batch) {
            w.join();
        }
    }
    CHECK_FALSE(overlapped.load());
    for (int t = 0; t < kPosters; ++t) {
        const std::vector<int>& got = seen[static_cast<std::size_t>(t)];
        REQUIRE(got.size() == static_cast<std::size_t>(kPostsEach));
        for (int i = 0; i < kPostsEach; ++i) {
            CHECK(got[static_cast<std::size_t>(i)] == i);
        }
    }
}
