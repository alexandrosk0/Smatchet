#include <doctest/doctest.h>

#include "OfflineFieldConflictPolicy.h"

using OfflineFieldConflictPolicy::ServerMovedFromBase;
using OfflineFieldConflictPolicy::TrimWhitespace;

TEST_CASE("ServerMovedFromBase — server unchanged → no conflict") {
    // base == theirs → server has not moved → replay (false).
    CHECK_FALSE(ServerMovedFromBase("In Progress", "In Progress"));
    CHECK_FALSE(ServerMovedFromBase("10001", "10001"));
    CHECK_FALSE(ServerMovedFromBase("", ""));
}

TEST_CASE("ServerMovedFromBase — server moved → conflict") {
    // base != theirs → server moved since the user looked → ask (true).
    CHECK(ServerMovedFromBase("In Progress", "Done"));
    CHECK(ServerMovedFromBase("alice", "bob"));
    CHECK(ServerMovedFromBase("High", "Low"));
}

TEST_CASE("ServerMovedFromBase — whitespace-only difference is not a conflict") {
    // Leading/trailing whitespace drift from re-serialization must not false-conflict.
    CHECK_FALSE(ServerMovedFromBase("In Progress", "  In Progress  "));
    CHECK_FALSE(ServerMovedFromBase("Done\n", "Done"));
    CHECK_FALSE(ServerMovedFromBase("\t10001 ", "10001"));
    // Interior whitespace is NOT normalized — a real semantic difference.
    CHECK(ServerMovedFromBase("In Progress", "InProgress"));
}

TEST_CASE("ServerMovedFromBase — empty base → no detection (documented residue)") {
    // No reference captured → cannot ask → replay (last-write-wins residue).
    CHECK_FALSE(ServerMovedFromBase("", "Done"));
    CHECK_FALSE(ServerMovedFromBase("   ", "Done")); // all-whitespace base trims to empty
}

TEST_CASE("ServerMovedFromCapturedBase — presence flag drives detection (ADR-0016)") {
    using OfflineFieldConflictPolicy::ServerMovedFromCapturedBase;
    // hasBase=false → never detect (legacy / no-base residue), regardless of values.
    CHECK_FALSE(ServerMovedFromCapturedBase(false, "", "Done"));
    CHECK_FALSE(ServerMovedFromCapturedBase(false, "In Progress", "Done"));
    // hasBase=true → a captured BLANK base is a real reference: blank→non-blank IS a move.
    CHECK(ServerMovedFromCapturedBase(true, "", "Done"));
    CHECK(ServerMovedFromCapturedBase(true, "In Progress", "Done"));
    // hasBase=true, unchanged (incl. blank==blank after trim) → not a move.
    CHECK_FALSE(ServerMovedFromCapturedBase(true, "Done", "Done"));
    CHECK_FALSE(ServerMovedFromCapturedBase(true, "", ""));
    CHECK_FALSE(ServerMovedFromCapturedBase(true, "  ", "")); // both trim to empty
}

TEST_CASE("TrimWhitespace — strips leading + trailing ASCII whitespace") {
    CHECK(TrimWhitespace("  hi  ") == "hi");
    CHECK(TrimWhitespace("\t\nx\r\n") == "x");
    CHECK(TrimWhitespace("no-trim") == "no-trim");
    CHECK(TrimWhitespace("   ").empty());
    CHECK(TrimWhitespace("").empty());
}
