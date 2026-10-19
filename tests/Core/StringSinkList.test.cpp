// StringSinkList — the copy-on-write callback list behind AppController's automation error sinks and
// LuaAutomationHost's log sinks (#2307). The concurrent cases are what the TSan lane is for: they
// register and clear sinks while other threads invoke them, the shape that raced when the lists were
// plain vectors appended on the UI thread and iterated on the automation worker.

#include "StringSinkList.h"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using smatchet::StringSinkList;

TEST_CASE("StringSinkList calls sinks in registration order and ignores empty functions") {
    StringSinkList list;
    std::vector<std::string> calls;
    CHECK_FALSE(list.InvokeAll("nobody"));
    list.Add([&calls](const std::string& m) { calls.push_back("a:" + m); });
    list.Add(StringSinkList::Sink());
    list.Add([&calls](const std::string& m) { calls.push_back("b:" + m); });
    CHECK(list.Snapshot()->size() == 2u);
    CHECK(list.InvokeAll("x"));
    REQUIRE(calls.size() == 2u);
    CHECK(calls[0] == "a:x");
    CHECK(calls[1] == "b:x");
}

TEST_CASE("StringSinkList snapshots are immutable: later Add and Clear do not change them") {
    StringSinkList list;
    int first = 0;
    list.Add([&first](const std::string&) { ++first; });
    const std::shared_ptr<const StringSinkList::Sinks> before = list.Snapshot();
    list.Add([](const std::string&) {});
    list.Clear();
    CHECK(before->size() == 1u);
    (*before)[0]("still callable");
    CHECK(first == 1);
    CHECK(list.Snapshot()->empty());
    CHECK_FALSE(list.InvokeAll("after clear"));
}

TEST_CASE("StringSinkList: a sink may register another sink while being invoked (no lock held)") {
    StringSinkList list;
    int added = 0;
    list.Add([&list, &added](const std::string&) {
        list.Add([](const std::string&) {});
        ++added;
    });
    CHECK(list.InvokeAll("once"));
    CHECK(added == 1);
    CHECK(list.Snapshot()->size() == 2u);
}

TEST_CASE("StringSinkList: registration and clearing race invocation without a data race") {
    StringSinkList list;
    std::atomic<bool> stop(false);
    std::atomic<long> invoked(0);
    std::vector<std::thread> invokers;
    for (int t = 0; t < 3; ++t) {
        invokers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                list.InvokeAll("line");
            }
        });
    }
    // The registering side: append sinks (each reallocates a plain vector) and clear the list, as
    // plugin init, debug.lua_log_test and host teardown do while the automation worker logs.
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 8; ++i) {
            list.Add([&invoked](const std::string&) { invoked.fetch_add(1, std::memory_order_relaxed); });
        }
        if (round % 10 == 9) {
            list.Clear();
        }
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& t : invokers) {
        t.join();
    }
    // The last round ends with a Clear, and every invocation ran against a whole, published list.
    CHECK(list.Snapshot()->empty());
    CHECK_FALSE(list.InvokeAll("done"));
}
