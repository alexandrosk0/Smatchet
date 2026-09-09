// Parent-issue hierarchy pure helpers (parent-issue-hierarchy plan): the seam the grid
// projection + the sync-time missing-parent fetch both lean on. Pure over
// std::vector<CachedTicket> — no ImGui, no HTTP, no SQLite.

#include "CachedTicketTypes.h"
#include "Tracker/ParentHierarchyPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

CachedTicket MakeTicket(const std::string& id, const std::string& parent = std::string()) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = "summary of " + id;
    if (!parent.empty()) {
        t.fieldValues["parent"] = parent;
    }
    return t;
}

bool ContainsIndex(const std::vector<size_t>& v, size_t idx) { return std::find(v.begin(), v.end(), idx) != v.end(); }

} // namespace

TEST_CASE("ParentHierarchyPure::ParentKeyFromFieldValue strips the ' - Summary' suffix") {
    CHECK(ParentHierarchyPure::ParentKeyFromFieldValue("PROJ-1 - Some summary") == "PROJ-1");
    CHECK(ParentHierarchyPure::ParentKeyFromFieldValue("PROJ-1") == "PROJ-1");
    CHECK(ParentHierarchyPure::ParentKeyFromFieldValue("  PROJ-7  ") == "PROJ-7");
    CHECK(ParentHierarchyPure::ParentKeyFromFieldValue("") == "");
    CHECK(ParentHierarchyPure::ParentKeyFromFieldValue("   ") == "");
}

TEST_CASE("ParentHierarchyPure::ParentKeyOf reads the parent field of a ticket") {
    CHECK(ParentHierarchyPure::ParentKeyOf(MakeTicket("A-2", "A-1 - Epic")) == "A-1");
    CHECK(ParentHierarchyPure::ParentKeyOf(MakeTicket("A-1")) == "");
}

TEST_CASE("ParentHierarchyPure::MissingParentKeys returns referenced-minus-present, deduped") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("A-1"));
    tickets.push_back(MakeTicket("A-2", "A-1"));
    tickets.push_back(MakeTicket("A-3", "EPIC-9 - Big epic"));
    tickets.push_back(MakeTicket("A-4", "EPIC-9"));
    tickets.push_back(MakeTicket("A-5", "EPIC-8"));

    const std::vector<std::string> missing = ParentHierarchyPure::MissingParentKeys(tickets);
    REQUIRE(missing.size() == 2);
    CHECK(missing[0] == "EPIC-9");
    CHECK(missing[1] == "EPIC-8");

    const std::vector<std::string> referenced = ParentHierarchyPure::ReferencedParentIds(tickets);
    REQUIRE(referenced.size() == 3);
    CHECK(referenced[0] == "A-1");
    CHECK(referenced[1] == "EPIC-9");
    CHECK(referenced[2] == "EPIC-8");
}

TEST_CASE("ParentHierarchyPure::PresentParentIds only lists parents that exist in the set") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("A-1"));
    tickets.push_back(MakeTicket("A-2", "A-1"));
    tickets.push_back(MakeTicket("A-3", "MISSING-1"));

    const std::unordered_set<std::string> present = ParentHierarchyPure::PresentParentIds(tickets);
    CHECK(present.size() == 1);
    CHECK(present.count("A-1") == 1);
    CHECK(present.count("MISSING-1") == 0);
    CHECK(present.count("A-3") == 0);
}

TEST_CASE("ParentHierarchyPure::ComputeDepths walks present ancestors; absent parent is root") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("A-1"));              // 0: root
    tickets.push_back(MakeTicket("A-2", "A-1"));       // 1: depth 1
    tickets.push_back(MakeTicket("A-3", "A-2"));       // 2: depth 2
    tickets.push_back(MakeTicket("A-4", "MISSING-1")); // 3: absent parent -> root
    tickets.push_back(MakeTicket("A-5"));              // 4: root

    const std::vector<int> depths = ParentHierarchyPure::ComputeDepths(tickets);
    REQUIRE(depths.size() == 5);
    CHECK(depths[0] == 0);
    CHECK(depths[1] == 1);
    CHECK(depths[2] == 2);
    CHECK(depths[3] == 0);
    CHECK(depths[4] == 0);
}

TEST_CASE("ParentHierarchyPure::ComputeDepths cuts a parent cycle instead of looping") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("C-1", "C-2"));
    tickets.push_back(MakeTicket("C-2", "C-1"));
    tickets.push_back(MakeTicket("C-3", "C-3"));

    const std::vector<int> depths = ParentHierarchyPure::ComputeDepths(tickets);
    REQUIRE(depths.size() == 3);
    for (int d : depths) {
        CHECK(d >= 0);
        CHECK(d <= ParentHierarchyPure::kMaxHierarchyDepth);
    }
}

TEST_CASE("ParentHierarchyPure::StoryGroupOrder places children right after their parent") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("A-1"));        // 0 root
    tickets.push_back(MakeTicket("B-1"));        // 1 root
    tickets.push_back(MakeTicket("A-2", "A-1")); // 2 child of 0
    tickets.push_back(MakeTicket("B-2", "B-1")); // 3 child of 1
    tickets.push_back(MakeTicket("A-3", "A-2")); // 4 grandchild of 0

    // Incoming order: sorted "B first" so root order must be preserved (B-1 before A-1).
    std::vector<size_t> order;
    order.push_back(1);
    order.push_back(3);
    order.push_back(0);
    order.push_back(4);
    order.push_back(2);

    const std::vector<size_t> grouped = ParentHierarchyPure::StoryGroupOrder(tickets, order);
    REQUIRE(grouped.size() == 5);
    CHECK(grouped[0] == 1); // B-1
    CHECK(grouped[1] == 3); // B-2 under B-1
    CHECK(grouped[2] == 0); // A-1
    // A-1's descendants follow it: A-2 then A-3 (A-3 is A-2's child).
    CHECK(grouped[3] == 2);
    CHECK(grouped[4] == 4);
}

TEST_CASE("ParentHierarchyPure::StoryGroupOrder drops out-of-range indices and keeps every "
          "in-range one exactly once") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("A-1"));
    tickets.push_back(MakeTicket("A-2", "A-1"));
    tickets.push_back(MakeTicket("A-3", "MISSING-1"));

    std::vector<size_t> order;
    order.push_back(2);
    order.push_back(99);
    order.push_back(1);
    order.push_back(0);

    const std::vector<size_t> grouped = ParentHierarchyPure::StoryGroupOrder(tickets, order);
    REQUIRE(grouped.size() == 3);
    CHECK(ContainsIndex(grouped, 0));
    CHECK(ContainsIndex(grouped, 1));
    CHECK(ContainsIndex(grouped, 2));
    CHECK_FALSE(ContainsIndex(grouped, 99));
    // A-3 (absent parent) is a root and keeps its leading position; A-2 sits behind A-1.
    CHECK(grouped[0] == 2);
    CHECK(grouped[1] == 0);
    CHECK(grouped[2] == 1);
}

TEST_CASE("ParentHierarchyPure::StoryGroupOrder treats cycle members as roots") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("C-1", "C-2"));
    tickets.push_back(MakeTicket("C-2", "C-1"));
    tickets.push_back(MakeTicket("C-3"));

    std::vector<size_t> order;
    order.push_back(0);
    order.push_back(1);
    order.push_back(2);

    const std::vector<size_t> grouped = ParentHierarchyPure::StoryGroupOrder(tickets, order);
    REQUIRE(grouped.size() == 3);
    CHECK(ContainsIndex(grouped, 0));
    CHECK(ContainsIndex(grouped, 1));
    CHECK(ContainsIndex(grouped, 2));
}

TEST_CASE("ParentHierarchyPure::AncestorChain lists present ancestors nearest-first") {
    std::vector<CachedTicket> tickets;
    tickets.push_back(MakeTicket("A-1"));              // 0
    tickets.push_back(MakeTicket("A-2", "A-1"));       // 1
    tickets.push_back(MakeTicket("A-3", "A-2"));       // 2
    tickets.push_back(MakeTicket("A-4", "MISSING-1")); // 3

    const std::vector<size_t> chain = ParentHierarchyPure::AncestorChain(tickets, 2);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0] == 1);
    CHECK(chain[1] == 0);

    CHECK(ParentHierarchyPure::AncestorChain(tickets, 0).empty());
    CHECK(ParentHierarchyPure::AncestorChain(tickets, 3).empty());
    CHECK(ParentHierarchyPure::AncestorChain(tickets, 42).empty());
}
