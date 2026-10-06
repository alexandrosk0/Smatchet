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

    std::atomic<bool> stop(false);
    std::atomic<int> mismatches(0);
    std::thread reader([&reg, &stop, &mismatches, knownPtr] {
        while (!stop.load(std::memory_order_acquire)) {
            const Command* c = reg.FindLocked("test.known");
            if (c != knownPtr || c->Summary != "known") {
                mismatches.fetch_add(1);
            }
            (void)reg.FindLocked("test.concurrent.250");
        }
    });
    // Enough inserts to force several rehashes of the registry map.
    for (int i = 0; i < 500; ++i) {
        reg.Register(MakeCommand("test.concurrent." + std::to_string(i), "concurrent " + std::to_string(i)));
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK(mismatches.load() == 0);
    CHECK(reg.FindLocked("test.known") == knownPtr);
    const Command* last = reg.FindLocked("test.concurrent.499");
    REQUIRE(last != nullptr);
    CHECK(last->Summary == "concurrent 499");
}
