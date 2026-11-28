// CacheEvictionPolicy.test.cpp — Phase 4 of
// docs/plans/shipped/memory-budget-and-lifetime-hardening.md.
//
// Unit-tests the pure CacheOverCap policy shared by the icon LRU texture cache and the AI plan
// FIFO cache: the decision to evict one more entry given count/byte caps, with no container,
// no GPU context, and no ImGui frame.

#include "CacheEvictionPolicy.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <map>
#include <string>

TEST_CASE("CacheOverCap: empty cache never asks to evict") {
    // count == 0 short-circuits to false even if a single item exceeds the byte cap, so a lone
    // oversized entry is admitted rather than looping forever against an empty cache.
    CHECK_FALSE(smatchet::CacheOverCap(0, 0, 10, 100));
    CHECK_FALSE(smatchet::CacheOverCap(0, 9999, 10, 100));
}

TEST_CASE("CacheOverCap: entry-count cap") {
    CHECK_FALSE(smatchet::CacheOverCap(10, 0, 10, 100)); // at cap, within bytes -> ok
    CHECK(smatchet::CacheOverCap(11, 0, 10, 100));       // over count cap -> evict
}

TEST_CASE("CacheOverCap: aggregate-byte cap") {
    CHECK_FALSE(smatchet::CacheOverCap(2, 100, 10, 100)); // at byte cap -> ok
    CHECK(smatchet::CacheOverCap(2, 101, 10, 100));       // over byte cap -> evict
}

TEST_CASE("CacheOverCap: either cap alone triggers eviction") {
    CHECK(smatchet::CacheOverCap(11, 50, 10, 100));  // count over only
    CHECK(smatchet::CacheOverCap(5, 500, 10, 100));  // bytes over only
    CHECK(smatchet::CacheOverCap(11, 500, 10, 100)); // both over
}

TEST_CASE("CacheOverCap: evict-before-insert call shape (simulated post-insert figures)") {
    // The icon cache passes count+1 / bytes+incoming so the loop makes room for the new entry.
    // 384 resident + 1 incoming == 385 > 384 -> must evict first.
    CHECK(smatchet::CacheOverCap(384 + 1, 0, 384, 96u * 1024u * 1024u));
    // 383 resident + 1 incoming == 384, within cap -> no evict needed.
    CHECK_FALSE(smatchet::CacheOverCap(383 + 1, 0, 384, 96u * 1024u * 1024u));
}

TEST_CASE("SweepDue: only over the cap, and only once per spacing window") {
    using smatchet::SweepDue;
    CHECK_FALSE(SweepDue(10, 10, 1000, 0, 300)); // at the cap: nothing to sweep
    CHECK_FALSE(SweepDue(0, 10, 1000, 0, 300));
    CHECK(SweepDue(11, 10, 1000, 0, 300));          // over the cap, long since the last sweep
    CHECK_FALSE(SweepDue(11, 10, 1100, 1000, 300)); // swept 100 ticks ago: wait (hysteresis)
    CHECK_FALSE(SweepDue(11, 10, 1299, 1000, 300)); // one tick short of the spacing
    CHECK(SweepDue(11, 10, 1300, 1000, 300));       // spacing elapsed
    CHECK(SweepDue(11, 10, 5, 1000, 300));          // tick counter went backwards: treat as due
}

TEST_CASE("EraseUnusedSince: erases entries older than the cutoff, keeps the rest, reports the count") {
    struct Entry {
        std::uint64_t LastUsed;
    };
    std::map<std::string, Entry> m;
    m["old"] = Entry{10};
    m["edge"] = Entry{100}; // exactly at the cutoff: kept
    m["new"] = Entry{500};
    m["older"] = Entry{0};
    const std::size_t erased = smatchet::EraseUnusedSince(m, 100, [](const Entry& e) { return e.LastUsed; });
    CHECK(erased == 2);
    CHECK(m.size() == 2);
    CHECK(m.count("edge") == 1);
    CHECK(m.count("new") == 1);
    CHECK(smatchet::EraseUnusedSince(m, 0, [](const Entry& e) { return e.LastUsed; }) == 0);
    CHECK(smatchet::EraseUnusedSince(m, 1000, [](const Entry& e) { return e.LastUsed; }) == 2);
    CHECK(m.empty());
}
