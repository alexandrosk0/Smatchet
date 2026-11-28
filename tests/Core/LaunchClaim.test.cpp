#include <doctest/doctest.h>

#include "LaunchClaim.h"

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

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
