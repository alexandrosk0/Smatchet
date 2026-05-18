// HarnessRunState — pure FSM helpers. Covers every allowed transition,
// rejection of every disallowed transition, and the round-trip of the
// string<->enum helpers. The FSM is load-bearing — loosening any rule
// here defeats the controller's integrity boundary.

#include "HarnessRunState.h"

#include <doctest/doctest.h>

#include <string>

using CodingHarness::RunState;

TEST_CASE("HarnessRunState — RunStateToString covers every enumerator") {
    CHECK(std::string(CodingHarness::RunStateToString(RunState::Pending)) == "Pending");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::Spawning)) == "Spawning");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::Running)) == "Running");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::AwaitingUser)) == "AwaitingUser");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::PrOpen)) == "PrOpen");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::Iterating)) == "Iterating");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::Complete)) == "Complete");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::Failed)) == "Failed");
    CHECK(std::string(CodingHarness::RunStateToString(RunState::Cancelled)) == "Cancelled");
}

TEST_CASE("HarnessRunState — ParseRunState round-trips every enumerator") {
    const RunState all[] = {RunState::Pending,      RunState::Spawning,  RunState::Running,
                            RunState::AwaitingUser, RunState::PrOpen,    RunState::Iterating,
                            RunState::Complete,     RunState::Failed,    RunState::Cancelled};
    for (RunState s : all) {
        RunState parsed = RunState::Pending; // arbitrary init
        REQUIRE(CodingHarness::ParseRunState(CodingHarness::RunStateToString(s), parsed));
        CHECK(parsed == s);
    }
}

TEST_CASE("HarnessRunState — ParseRunState rejects unknown literals") {
    RunState out = RunState::Pending;
    CHECK_FALSE(CodingHarness::ParseRunState("", out));
    CHECK_FALSE(CodingHarness::ParseRunState("Unknown", out));
    CHECK_FALSE(CodingHarness::ParseRunState("running", out)); // case-sensitive
    CHECK_FALSE(CodingHarness::ParseRunState("Done", out));
    // Output untouched on rejection.
    CHECK(out == RunState::Pending);
}

TEST_CASE("HarnessRunState — IsTerminal flags exactly Complete / Failed / Cancelled") {
    CHECK(CodingHarness::IsTerminal(RunState::Complete));
    CHECK(CodingHarness::IsTerminal(RunState::Failed));
    CHECK(CodingHarness::IsTerminal(RunState::Cancelled));
    CHECK_FALSE(CodingHarness::IsTerminal(RunState::Pending));
    CHECK_FALSE(CodingHarness::IsTerminal(RunState::Spawning));
    CHECK_FALSE(CodingHarness::IsTerminal(RunState::Running));
    CHECK_FALSE(CodingHarness::IsTerminal(RunState::AwaitingUser));
    CHECK_FALSE(CodingHarness::IsTerminal(RunState::PrOpen));
    CHECK_FALSE(CodingHarness::IsTerminal(RunState::Iterating));
}

TEST_CASE("HarnessRunState — Pending only transitions to Spawning") {
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::Spawning));
    // Every other target is rejected.
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::Pending));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::Running));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::AwaitingUser));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::PrOpen));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::Iterating));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::Complete));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::Failed));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Pending, RunState::Cancelled));
}

TEST_CASE("HarnessRunState — Spawning transitions are {Running, Failed}") {
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::Running));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::Failed));
    // Other targets rejected.
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::Pending));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::Spawning));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::AwaitingUser));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::PrOpen));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::Complete));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::Iterating));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Spawning, RunState::Cancelled));
}

TEST_CASE("HarnessRunState — Running transitions are {AwaitingUser, PrOpen, Failed, Cancelled}") {
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::AwaitingUser));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::PrOpen));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::Failed));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::Cancelled));
    // Rejected.
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::Pending));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::Spawning));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::Running));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::Complete));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Running, RunState::Iterating));
}

TEST_CASE("HarnessRunState — AwaitingUser transitions are {Running, Failed, Cancelled}") {
    CHECK(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::Running));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::Failed));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::Cancelled));
    // Rejected.
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::Pending));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::Spawning));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::AwaitingUser));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::PrOpen));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::Iterating));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::AwaitingUser, RunState::Complete));
}

TEST_CASE("HarnessRunState — PrOpen transitions are {Iterating, Complete, Failed, Cancelled}") {
    CHECK(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::Iterating));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::Complete));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::Failed));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::Cancelled));
    // Rejected.
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::Pending));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::Spawning));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::Running));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::AwaitingUser));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::PrOpen, RunState::PrOpen));
}

TEST_CASE("HarnessRunState — Iterating transitions are {PrOpen, Complete, Failed, Cancelled}") {
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::PrOpen));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::Complete));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::Failed));
    CHECK(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::Cancelled));
    // Rejected.
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::Pending));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::Spawning));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::Running));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::AwaitingUser));
    CHECK_FALSE(CodingHarness::IsTransitionAllowed(RunState::Iterating, RunState::Iterating));
}

TEST_CASE("HarnessRunState — terminal states reject every outbound transition") {
    const RunState terminals[] = {RunState::Complete, RunState::Failed, RunState::Cancelled};
    const RunState every[] = {RunState::Pending,      RunState::Spawning,  RunState::Running,
                              RunState::AwaitingUser, RunState::PrOpen,    RunState::Iterating,
                              RunState::Complete,     RunState::Failed,    RunState::Cancelled};
    for (RunState from : terminals) {
        for (RunState to : every) {
            INFO("terminal from=", CodingHarness::RunStateToString(from), " to=",
                 CodingHarness::RunStateToString(to));
            CHECK_FALSE(CodingHarness::IsTransitionAllowed(from, to));
        }
    }
}
