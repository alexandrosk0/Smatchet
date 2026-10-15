#include <doctest/doctest.h>

#include "Sync/RefusedKeysPure.h"

#include <string>

using smatchet::sync::RefusedKeySet;

TEST_CASE("RefusedKeySet: a key is remembered until it is forgotten") {
    RefusedKeySet set;
    CHECK(set.Empty());
    set.Insert("ABC-1", "T1", 100);
    CHECK(set.Contains("ABC-1"));
    CHECK_FALSE(set.Contains("ABC-2"));
    CHECK(set.Size() == 1u);
    set.Clear();
    CHECK(set.Empty());
}

TEST_CASE("RefusedKeySet: the oldest keys are dropped past the cap") {
    RefusedKeySet set;
    const std::size_t cap = RefusedKeySet::MaxKeys();
    CHECK(cap == 256u);
    for (std::size_t i = 0; i < cap + 10; ++i) {
        set.Insert("K-" + std::to_string(i), "T1", 100);
    }
    CHECK(set.Size() == cap);
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK_FALSE(set.Contains("K-" + std::to_string(i))); // the first ten went
    }
    CHECK(set.Contains("K-10"));
    CHECK(set.Contains("K-" + std::to_string(cap + 9)));
}

TEST_CASE("RefusedKeySet: refreshing a key makes it the newest, so it survives the cap") {
    RefusedKeySet set;
    const std::size_t cap = RefusedKeySet::MaxKeys();
    for (std::size_t i = 0; i < cap; ++i) {
        set.Insert("K-" + std::to_string(i), "T1", 100);
    }
    set.Insert("K-0", "T2", 200); // refused again: now the newest
    set.Insert("NEW", "T1", 200); // one over the cap: the oldest (K-1) goes, not K-0
    CHECK(set.Size() == cap);
    CHECK(set.Contains("K-0"));
    CHECK_FALSE(set.Contains("K-1"));
}

TEST_CASE("RefusedKeySet: entries expire after the max age, and a non-positive age never expires them") {
    RefusedKeySet set;
    set.Insert("OLD", "T1", 100);
    set.Insert("FRESH", "T1", 900);
    CHECK(set.ExpireOlderThan(1000, 0) == 0u); // age 0 = never
    CHECK(set.ExpireOlderThan(1000, -5) == 0u);
    CHECK(set.Size() == 2u);
    CHECK(set.ExpireOlderThan(1000, 500) == 1u); // OLD is 900 s old, FRESH 100 s
    CHECK_FALSE(set.Contains("OLD"));
    CHECK(set.Contains("FRESH"));
    CHECK(set.ExpireOlderThan(1000, 500) == 0u);
}

TEST_CASE(
    "RefusedKeySet::NoteServed: a body at the refused version or a newer one clears the key, an older one does not") {
    RefusedKeySet set;
    set.Insert("ABC-1", "2024-05-01T10:00:00.000+0000", 100);
    set.Insert("ABC-2", "2024-05-01T10:00:00.000+0000", 100);
    set.Insert("ABC-3", "2024-05-01T10:00:00.000+0000", 100);

    CHECK_FALSE(set.NoteServed("NOT-HELD", "2024-05-01T10:00:00.000+0000"));
    CHECK_FALSE(set.NoteServed("ABC-1", "2024-04-30T10:00:00.000+0000")); // older: the lagging streamed copy
    CHECK(set.Contains("ABC-1"));
    CHECK(set.NoteServed("ABC-2", "2024-05-01T12:00:00.000+0200")); // the same instant, another zone spelling
    CHECK_FALSE(set.Contains("ABC-2"));
    CHECK(set.NoteServed("ABC-3", "2024-05-02T10:00:00.000+0000")); // newer
    CHECK_FALSE(set.Contains("ABC-3"));
}

TEST_CASE("RefusedKeySet::NoteServed: stamps that do not parse count as the same version only when equal") {
    RefusedKeySet set;
    set.Insert("ABC-1", "T2", 100);
    CHECK_FALSE(set.NoteServed("ABC-1", "T1")); // no order can be claimed: the memory stays
    CHECK(set.Contains("ABC-1"));
    CHECK(set.NoteServed("ABC-1", "T2"));
    CHECK(set.Empty());
}
