// GridPaneEvictionPolicy.test.cpp — multi-grid-tabs.md Slice 5a (plan item 21).
//
// Unit-tests the pure ChooseHiddenPaneToEvict decision for the hidden-pane LRU memory cap:
// LRU order (least-recently-visible first), visible-never-evicted, busy-sync-never-evicted,
// under-cap no-op, and the deterministic paneId tie-break — with no AppController, no SQLite
// cache, and no ImGui frame.

#include "GridPaneEvictionPolicy.h"

#include "doctest/doctest.h"

using smatchet::ChooseHiddenPaneToEvict;
using smatchet::HiddenPaneEvictionCandidate;

namespace {
HiddenPaneEvictionCandidate Hidden(const std::string& id, std::uint64_t order) {
    HiddenPaneEvictionCandidate c;
    c.paneId = id;
    c.isVisible = false;
    c.hasActiveSync = false;
    c.hasResidentRows = true;
    c.lastVisibleOrder = order;
    return c;
}
} // namespace

TEST_CASE("ChooseHiddenPaneToEvict: under cap is a no-op") {
    std::vector<HiddenPaneEvictionCandidate> panes = {Hidden("a", 1), Hidden("b", 2)};
    // 2 hidden resident, cap 4 -> nothing to drop.
    CHECK(ChooseHiddenPaneToEvict(panes, 4).empty());
    // Exactly at cap (CacheOverCap convention: count > cap evicts).
    CHECK(ChooseHiddenPaneToEvict(panes, 2).empty());
}

TEST_CASE("ChooseHiddenPaneToEvict: over cap drops the least-recently-visible") {
    std::vector<HiddenPaneEvictionCandidate> panes = {Hidden("a", 30), Hidden("b", 10), Hidden("c", 20)};
    // 3 hidden resident, cap 2 -> evict the smallest order (b@10).
    CHECK(ChooseHiddenPaneToEvict(panes, 2) == "b");
}

TEST_CASE("ChooseHiddenPaneToEvict: visible panes are never evicted and never counted") {
    std::vector<HiddenPaneEvictionCandidate> panes;
    HiddenPaneEvictionCandidate vis = Hidden("visible", 1); // oldest order, but visible
    vis.isVisible = true;
    panes.push_back(vis);
    panes.push_back(Hidden("h1", 50));
    panes.push_back(Hidden("h2", 60));
    // Only 2 hidden resident (visible excluded) -> at cap 2, no eviction even though the
    // visible pane has the oldest recency stamp.
    CHECK(ChooseHiddenPaneToEvict(panes, 2).empty());
    // Drop cap to 1: must pick the older HIDDEN pane (h1@50), never the visible one.
    CHECK(ChooseHiddenPaneToEvict(panes, 1) == "h1");
}

TEST_CASE("ChooseHiddenPaneToEvict: busy-sync panes count toward cap but are not evicted") {
    std::vector<HiddenPaneEvictionCandidate> panes;
    HiddenPaneEvictionCandidate busy = Hidden("busy", 1); // oldest, but mid-sync
    busy.hasActiveSync = true;
    panes.push_back(busy);
    panes.push_back(Hidden("idle", 99));
    // 2 hidden resident over cap 1 -> the oldest (busy) is skipped, the idle one is evicted
    // even though it is more recently visible (Pillar 3: never free rows a worker is touching).
    CHECK(ChooseHiddenPaneToEvict(panes, 1) == "idle");
}

TEST_CASE("ChooseHiddenPaneToEvict: all over-cap candidates busy -> none this frame") {
    std::vector<HiddenPaneEvictionCandidate> panes = {Hidden("x", 1), Hidden("y", 2)};
    panes[0].hasActiveSync = true;
    panes[1].hasActiveSync = true;
    // Over cap 1, but nothing idle to evict -> defer (retried next frame).
    CHECK(ChooseHiddenPaneToEvict(panes, 1).empty());
}

TEST_CASE("ChooseHiddenPaneToEvict: already-evicted husks do not count") {
    std::vector<HiddenPaneEvictionCandidate> panes = {Hidden("a", 1), Hidden("b", 2), Hidden("c", 3)};
    panes[0].hasResidentRows = false; // already evicted earlier
    // Only 2 resident now -> at cap 2, no further eviction.
    CHECK(ChooseHiddenPaneToEvict(panes, 2).empty());
}

TEST_CASE("ChooseHiddenPaneToEvict: equal recency breaks ties by smaller paneId") {
    std::vector<HiddenPaneEvictionCandidate> panes = {Hidden("zeta", 5), Hidden("alpha", 5), Hidden("mid", 5)};
    CHECK(ChooseHiddenPaneToEvict(panes, 2) == "alpha");
}

TEST_CASE("ChooseHiddenPaneToEvict: loop evicts down to the cap") {
    std::vector<HiddenPaneEvictionCandidate> panes = {Hidden("a", 1), Hidden("b", 2), Hidden("c", 3), Hidden("d", 4)};
    // Simulate the caller loop: evict (flip hasResidentRows) until at cap 2.
    std::vector<std::string> evicted;
    for (;;) {
        const std::string victim = ChooseHiddenPaneToEvict(panes, 2);
        if (victim.empty()) {
            break;
        }
        evicted.push_back(victim);
        for (std::size_t i = 0; i < panes.size(); ++i) {
            if (panes[i].paneId == victim) {
                panes[i].hasResidentRows = false;
            }
        }
    }
    // Oldest two (a@1, b@2) dropped, in recency order; c/d retained.
    REQUIRE(evicted.size() == 2);
    CHECK(evicted[0] == "a");
    CHECK(evicted[1] == "b");
}
