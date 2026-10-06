// CommandRegistry::FindLocked runs on the UI thread every frame while a worker can register a
// command (a Lua snippet calling ui.register_global_action off the UI thread). This lives in the
// TSan subset so ThreadSanitizer, not a lucky mismatch, catches an unlocked lookup.

#include <doctest/doctest.h>

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <utility>

using smatchet::cmd::Command;
using smatchet::cmd::CommandContext;
using smatchet::cmd::CommandRegistry;
using smatchet::cmd::CommandResult;

namespace {

Command MakeCommand(const std::string& name, const std::string& summary) {
    Command c;
    c.Name = name;
    c.Summary = summary;
    c.Handler = [](const nlohmann::json&, const CommandContext&) {
        return CommandResult::Success(nlohmann::json::object());
    };
    return c;
}

} // namespace

TEST_CASE("CommandRegistry: FindLocked races Register safely and its pointers stay valid") {
    CommandRegistry reg;
    reg.Register(MakeCommand("test.known", "known"));
    const Command* knownPtr = reg.FindLocked("test.known");
    REQUIRE(knownPtr != nullptr);

    // The reader's progress is published with relaxed atomics on purpose: they pace the writer so
    // lookups really overlap the inserts, without adding the happens-before edges that would hide an
    // unlocked lookup from ThreadSanitizer.
    std::atomic<bool> stop(false);
    std::atomic<int> mismatches(0);
    std::atomic<long> lookups(0);
    std::thread reader([&reg, &stop, &mismatches, &lookups, knownPtr] {
        while (!stop.load(std::memory_order_acquire)) {
            const Command* c = reg.FindLocked("test.known");
            if (c != knownPtr || c->Summary != "known") {
                mismatches.fetch_add(1);
            }
            (void)reg.FindLocked("test.concurrent.250");
            lookups.fetch_add(1, std::memory_order_relaxed);
        }
    });
    // Waits (bounded) until the reader has done more lookups than `seen`.
    auto waitForReader = [&lookups](long seen) {
        for (int spin = 0; spin < 200000 && lookups.load(std::memory_order_relaxed) <= seen; ++spin) {
            std::this_thread::yield();
        }
        return lookups.load(std::memory_order_relaxed);
    };
    long seen = waitForReader(0);
    // Enough inserts to force several rehashes of the registry map, with the reader running throughout.
    for (int i = 0; i < 500; ++i) {
        reg.Register(MakeCommand("test.concurrent." + std::to_string(i), "concurrent " + std::to_string(i)));
        if (i % 25 == 0) {
            seen = waitForReader(seen);
        }
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK(lookups.load() > 20); // the reader overlapped the inserts, so the case is not vacuous
    CHECK(mismatches.load() == 0);
    CHECK(reg.FindLocked("test.known") == knownPtr);
    const Command* last = reg.FindLocked("test.concurrent.499");
    REQUIRE(last != nullptr);
    CHECK(last->Summary == "concurrent 499");
}
