#include <doctest/doctest.h>

#include "LaunchClaim.h"
#include "SerialTaskQueue.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
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

TEST_CASE("SerialTaskQueue::Flush returns at once on an idle queue") {
    SerialTaskQueue queue([](std::function<void()> drain) { drain(); });
    CHECK(queue.Flush());
    CHECK(queue.Post([]() {}).empty());
    CHECK(queue.Flush(std::chrono::milliseconds(10)));
}

TEST_CASE("SerialTaskQueue::Flush waits for the worker to finish every posted task") {
    std::vector<std::thread> workers;
    SerialTaskQueue queue([&workers](std::function<void()> drain) { workers.emplace_back(std::move(drain)); });
    std::atomic<int> done{0};
    for (int i = 0; i < 5; ++i) {
        CHECK(queue
                  .Post([&done]() {
                      std::this_thread::sleep_for(std::chrono::milliseconds(5));
                      done.fetch_add(1);
                  })
                  .empty());
    }
    CHECK(queue.Flush());
    CHECK(done.load() == 5);
    for (std::thread& w : workers) {
        w.join();
    }
}

TEST_CASE("SerialTaskQueue::Flush times out while a task is still running") {
    std::vector<std::thread> workers;
    SerialTaskQueue queue([&workers](std::function<void()> drain) { workers.emplace_back(std::move(drain)); });
    std::atomic<bool> release{false};
    CHECK(queue
              .Post([&release]() {
                  while (!release.load()) {
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  }
              })
              .empty());
    CHECK_FALSE(queue.Flush(std::chrono::milliseconds(20)));
    release.store(true);
    CHECK(queue.Flush());
    for (std::thread& w : workers) {
        w.join();
    }
}

TEST_CASE("SerialTaskQueue::Flush drains a task a failed launch left behind on the calling thread") {
    SerialTaskQueue queue([](std::function<void()>) { throw std::runtime_error("no threads"); });
    int ran = 0;
    CHECK(queue.Post([&ran]() { ++ran; }) == "no threads");
    CHECK(ran == 0);
    CHECK(queue.Flush());
    CHECK(ran == 1);
}

TEST_CASE("SerialTaskQueue relaunches after the launcher dropped a drain unrun") {
    // A launcher that destroys the closure without running it (AppController::LaunchBackgroundTask does
    // this once shutdown has begun) must not leave the queue believing a drain is running.
    int dropped = 0;
    bool drop = true;
    std::vector<std::function<void()>> launched;
    SerialTaskQueue queue([&](std::function<void()> drain) {
        if (drop) {
            ++dropped;
            return; // `drain` is destroyed here, unrun
        }
        launched.push_back(std::move(drain));
    });
    std::vector<int> ran;
    CHECK(queue.Post([&ran]() { ran.push_back(1); }).empty());
    CHECK(dropped == 1);
    CHECK(ran.empty());
    drop = false;
    CHECK(queue.Post([&ran]() { ran.push_back(2); }).empty());
    REQUIRE(launched.size() == 1); // the second post started a drain: the latch was not stuck
    launched[0]();
    CHECK(ran == std::vector<int>{1, 2}); // first-in, first-run
}

TEST_CASE("SerialTaskQueue::Flush runs what a dropped drain stranded, on the calling thread") {
    SerialTaskQueue queue([](std::function<void()>) { /* dropped unrun */ });
    int ran = 0;
    CHECK(queue.Post([&ran]() { ++ran; }).empty());
    CHECK(queue.Post([&ran]() { ++ran; }).empty()); // a second post relaunches (and is dropped) too
    CHECK(ran == 0);
    CHECK(queue.Flush(std::chrono::milliseconds(50)));
    CHECK(ran == 2);
}

TEST_CASE("SerialTaskQueue: a drain that runs after its claim was released does nothing") {
    // A launcher that throws after stashing the closure: the catch released the latch, so a later run of
    // the stashed closure must not drain concurrently with the drain a newer post started.
    std::function<void()> stashed;
    int launches = 0;
    SerialTaskQueue queue([&](std::function<void()> drain) {
        ++launches;
        if (launches == 1) {
            stashed = std::move(drain);
            throw std::runtime_error("launch failed after queueing");
        }
        drain();
    });
    std::vector<int> ran;
    CHECK(queue.Post([&ran]() { ran.push_back(1); }) == "launch failed after queueing");
    REQUIRE(stashed);
    CHECK(queue.Post([&ran]() { ran.push_back(2); }).empty());
    CHECK(ran == std::vector<int>{1, 2});
    stashed(); // the first launch's closure running late: its claim is gone, so it runs nothing
    CHECK(ran == std::vector<int>{1, 2});
    CHECK(queue.Post([&ran]() { ran.push_back(3); }).empty());
    CHECK(ran == std::vector<int>{1, 2, 3});
}

TEST_CASE("SerialTaskQueue releases its latch and rethrows when the launch throws a non-std exception") {
    bool throwOdd = true;
    SerialTaskQueue queue([&throwOdd](std::function<void()> drain) {
        if (throwOdd) {
            throw 7;
        }
        drain();
    });
    int ran = 0;
    CHECK_THROWS(queue.Post([&ran]() { ++ran; }));
    throwOdd = false;
    CHECK(queue.Post([&ran]() { ++ran; }).empty()); // not stuck: the first task runs ahead of this one
    CHECK(ran == 2);
}

TEST_CASE("SerialTaskQueue: a failing first launch racing concurrent posts never runs two drains at once") {
    for (int round = 0; round < 50; ++round) {
        std::atomic<int> launchCount{0};
        std::mutex threadsMutex;
        std::vector<std::thread> workers;
        SerialTaskQueue queue([&](std::function<void()> drain) {
            if (launchCount.fetch_add(1) == 0) {
                throw std::runtime_error("first launch fails");
            }
            std::lock_guard<std::mutex> lock(threadsMutex);
            workers.emplace_back(std::move(drain));
        });
        std::atomic<int> active{0};
        std::atomic<bool> overlapped{false};
        std::vector<int> seen; // written only inside tasks, which never overlap
        std::vector<std::thread> posters;
        for (int t = 0; t < 4; ++t) {
            posters.emplace_back([&]() {
                for (int i = 0; i < 25; ++i) {
                    queue.Post([&]() {
                        if (active.fetch_add(1) != 0) {
                            overlapped.store(true);
                        }
                        seen.push_back(0);
                        active.fetch_sub(1);
                    });
                }
            });
        }
        for (std::thread& p : posters) {
            p.join();
        }
        CHECK(queue.Flush());
        std::vector<std::thread> batch;
        {
            std::lock_guard<std::mutex> lock(threadsMutex);
            batch.swap(workers);
        }
        for (std::thread& w : batch) {
            w.join();
        }
        CHECK_FALSE(overlapped.load());
        CHECK(seen.size() == 100);
    }
}

// ---------------------------------------------------------------------------
// LaunchClaim — the single-owner release the queue's drain latch (and the lookup latches) rely on.
// ---------------------------------------------------------------------------

using smatchet::LaunchClaim;

TEST_CASE("LaunchClaim releases once when destroyed untaken") {
    int released = 0;
    {
        LaunchClaim claim([&released]() { ++released; });
        CHECK(released == 0);
    }
    CHECK(released == 1);
}

TEST_CASE("LaunchClaim does not release after a body took it") {
    int released = 0;
    {
        LaunchClaim claim([&released]() { ++released; });
        CHECK(claim.TakeForRun());
        claim.ReleaseIfUntaken();        // the launch catch after the body started: nothing to release
        CHECK_FALSE(claim.TakeForRun()); // a second taker never runs the body twice
    }
    CHECK(released == 0);
}

TEST_CASE("LaunchClaim: an explicit release wins once, forbids the body and the destructor repeats nothing") {
    int released = 0;
    {
        LaunchClaim claim([&released]() { ++released; });
        claim.ReleaseIfUntaken();
        claim.ReleaseIfUntaken();
        CHECK(released == 1);
        CHECK_FALSE(claim.TakeForRun()); // a task that runs late finds the claim already released
    }
    CHECK(released == 1);
}

TEST_CASE("LaunchClaim contains a release callable that throws and an empty one") {
    {
        LaunchClaim throwing([]() { throw std::runtime_error("release failed"); });
        CHECK_NOTHROW(throwing.ReleaseIfUntaken());
    }
    {
        LaunchClaim nothing{std::function<void()>()};
        CHECK_NOTHROW(nothing.ReleaseIfUntaken());
        CHECK_FALSE(nothing.TakeForRun());
    }
    {
        LaunchClaim throwsNonStd([]() { throw 42; });
        CHECK_NOTHROW(throwsNonStd.ReleaseIfUntaken());
    }
}

TEST_CASE("LaunchClaim: the last closure copy going away releases an unrun claim") {
    int released = 0;
    std::shared_ptr<LaunchClaim> claim = std::make_shared<LaunchClaim>([&released]() { ++released; });
    std::function<void()> closure = [claim]() { (void)claim->TakeForRun(); };
    std::function<void()> copy = closure;
    // The test's own reference plus the two closures: nothing releases while any holder is alive.
    closure = nullptr;
    copy = nullptr;
    CHECK(released == 0);
    // Only the local reference remains; dropping it releases.
    claim.reset();
    CHECK(released == 1);
}

TEST_CASE("LaunchClaim: a body and a releaser racing never both win") {
    for (int round = 0; round < 1000; ++round) {
        std::atomic<int> released{0};
        const std::shared_ptr<LaunchClaim> claim =
            std::make_shared<LaunchClaim>([&released]() { released.fetch_add(1); });
        std::atomic<bool> go{false};
        std::atomic<int> bodies{0};
        std::thread body([&]() {
            while (!go.load()) {
                std::this_thread::yield();
            }
            if (claim->TakeForRun()) {
                bodies.fetch_add(1);
            }
        });
        std::thread releaser([&]() {
            while (!go.load()) {
                std::this_thread::yield();
            }
            claim->ReleaseIfUntaken();
        });
        go.store(true);
        body.join();
        releaser.join();
        // Exactly one of the two paths ran: either the body or the release, never both, never neither.
        CHECK(bodies.load() + released.load() == 1);
    }
}

TEST_CASE("SerialTaskQueue::PendingCount counts queued and running tasks") {
    SerialTaskQueue queue([](std::function<void()>) { /* launch dropped: nothing runs */ });
    CHECK(queue.PendingCount() == 0);
    CHECK(queue.Post([]() {}).empty());
    CHECK(queue.PendingCount() == 1); // a refused launch keeps its task queued
    CHECK(queue.Post([]() {}).empty());
    CHECK(queue.PendingCount() == 2);
    CHECK(queue.Flush());
    CHECK(queue.PendingCount() == 0);
}

TEST_CASE("SerialTaskQueue::PendingCount includes the task that is running") {
    std::vector<std::thread> workers;
    SerialTaskQueue queue([&workers](std::function<void()> drain) { workers.emplace_back(std::move(drain)); });
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    CHECK(queue
              .Post([&]() {
                  started.store(true);
                  while (!release.load()) {
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  }
              })
              .empty());
    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(queue.PendingCount() == 1); // popped, executing
    CHECK(queue.Post([]() {}).empty());
    CHECK(queue.PendingCount() == 2);
    CHECK_FALSE(queue.Flush(std::chrono::milliseconds(20))); // residue under a short deadline
    CHECK(queue.PendingCount() == 2);
    release.store(true);
    CHECK(queue.Flush());
    CHECK(queue.PendingCount() == 0);
    for (std::thread& w : workers) {
        w.join();
    }
}

TEST_CASE("SerialTaskQueue::PendingCount drops the task that threw") {
    std::vector<std::function<void()>> launched;
    SerialTaskQueue queue([&launched](std::function<void()> drain) { launched.push_back(std::move(drain)); });
    CHECK(queue.Post([]() { throw std::runtime_error("disk gone"); }).empty());
    REQUIRE(launched.size() == 1);
    CHECK_THROWS_AS(launched[0](), std::runtime_error);
    CHECK(queue.PendingCount() == 0);
}

TEST_CASE("RemainingUntil never goes negative and shares one deadline") {
    const auto now = std::chrono::steady_clock::now();
    CHECK(smatchet::RemainingUntil(now - std::chrono::seconds(1)).count() == 0);
    const auto left = smatchet::RemainingUntil(now + std::chrono::seconds(60));
    CHECK(left.count() > 50000);
    CHECK(left.count() <= 60000);
}
