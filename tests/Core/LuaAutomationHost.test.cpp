// LuaAutomationHost — thin fan-out hub for plugin-registered Lua-log sinks.
// Pure (std::function only); no ImGui / AppController / sol2.

#include <doctest/doctest.h>

#include "LuaAutomationHost.h"

#include <string>

TEST_CASE("A fresh host has no sinks") {
    LuaAutomationHost host;
    CHECK(host.SnapshotLogSinks().empty());
}

TEST_CASE("AddAutomationLogSink registers a sink that receives lines") {
    LuaAutomationHost host;
    std::string captured;
    host.AddAutomationLogSink([&captured](const std::string& line) { captured = line; });

    auto sinks = host.SnapshotLogSinks();
    REQUIRE(sinks.size() == 1);
    sinks[0]("hello");
    CHECK(captured == "hello");
}

TEST_CASE("A null sink is ignored") {
    LuaAutomationHost host;
    host.AddAutomationLogSink(std::function<void(const std::string&)>());
    CHECK(host.SnapshotLogSinks().empty());
}

TEST_CASE("ClearAutomationLogSinks drops every sink") {
    LuaAutomationHost host;
    host.AddAutomationLogSink([](const std::string&) {});
    host.AddAutomationLogSink([](const std::string&) {});
    REQUIRE(host.SnapshotLogSinks().size() == 2);
    host.ClearAutomationLogSinks();
    CHECK(host.SnapshotLogSinks().empty());
}

TEST_CASE("SnapshotLogSinks returns an independent copy") {
    LuaAutomationHost host;
    host.AddAutomationLogSink([](const std::string&) {});
    auto snap = host.SnapshotLogSinks();
    host.ClearAutomationLogSinks();
    // The earlier snapshot still holds its sink — it is a copy, not a live view.
    CHECK(snap.size() == 1);
    CHECK(host.SnapshotLogSinks().empty());
}
