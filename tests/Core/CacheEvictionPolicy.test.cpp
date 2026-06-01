// CacheEvictionPolicy.test.cpp — Phase 4 of
// docs/plans/shipped/memory-budget-and-lifetime-hardening.md.
//
// Unit-tests the pure CacheOverCap policy shared by the icon LRU texture cache and the AI plan
// FIFO cache: the decision to evict one more entry given count/byte caps, with no container,
// no GPU context, and no ImGui frame.

#include "CacheEvictionPolicy.h"

#include "doctest/doctest.h"

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
