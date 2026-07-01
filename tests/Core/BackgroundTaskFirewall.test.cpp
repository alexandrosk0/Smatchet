// BackgroundTaskFirewall.test.cpp — issue #1081.
//
// Unit-tests the pure RunBackgroundTaskFirewalled helper that backs AppController's
// background-worker exception firewall (an exception escaping a worker-thread function calls
// std::terminate; the UI-thread SEH guard does not cover workers — Pillar-3 never-crash).
// Covers: clean completion, std::exception containment (what() captured), non-std containment,
// and the worker-lambda-shaped contract that the done-flag stored AFTER the firewalled run
// still publishes when the task throws (the reaper joins only done workers).

#include "BackgroundTaskFirewall.h"

#include "doctest/doctest.h"

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

TEST_CASE("RunBackgroundTaskFirewalled runs a clean task to completion") {
    bool ran = false;
    std::string what;
    const smatchet::BackgroundTaskOutcome outcome = smatchet::RunBackgroundTaskFirewalled([&]() { ran = true; }, what);
    CHECK(outcome == smatchet::BackgroundTaskOutcome::Completed);
    CHECK(ran);
    CHECK(what.empty());
}

TEST_CASE("RunBackgroundTaskFirewalled contains a std::exception and captures what()") {
    std::string what;
    const smatchet::BackgroundTaskOutcome outcome =
        smatchet::RunBackgroundTaskFirewalled([]() { throw std::runtime_error("boom"); }, what);
    CHECK(outcome == smatchet::BackgroundTaskOutcome::StdException);
    CHECK(what == "boom");
}

TEST_CASE("RunBackgroundTaskFirewalled contains a non-std exception") {
    std::string what;
    const smatchet::BackgroundTaskOutcome outcome = smatchet::RunBackgroundTaskFirewalled([]() { throw 42; }, what);
    CHECK(outcome == smatchet::BackgroundTaskOutcome::UnknownException);
    CHECK(what.empty());
}

TEST_CASE("done-flag stored after a throwing firewalled task still publishes") {
    // Worker-lambda-shaped: in AppController the done-flag store sits AFTER the firewalled
    // run and must execute even when the task throws — the reaper joins only done workers,
    // so a skipped store would leak the thread until shutdown. Exercised cross-thread like
    // the real pool: the worker runs firewall + release-store, the test thread joins and
    // acquire-loads (per a CR finding — the same-thread version was tautological).
    std::atomic<bool> done(false);
    smatchet::BackgroundTaskOutcome outcome = smatchet::BackgroundTaskOutcome::Completed;
    std::string what;
    std::thread worker([&]() {
        outcome = smatchet::RunBackgroundTaskFirewalled([]() { throw std::runtime_error("mid-task failure"); }, what);
        done.store(true, std::memory_order_release);
    });
    worker.join();
    CHECK(outcome == smatchet::BackgroundTaskOutcome::StdException);
    CHECK(done.load(std::memory_order_acquire));
}
