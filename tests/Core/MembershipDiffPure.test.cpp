// Bucket-A coverage for MembershipDiffPure (ticket-change-monitor plan, S1a) — the pure
// set-difference the per-pane change monitor uses to reconcile its tracked set against the
// view's freshly-fetched key list. RemovedKeys -> candidates for the left-view/deleted probe;
// AddedKeys -> keys that newly entered the view. Header-only (no production .cpp to link).

#include "Sync/MembershipDiffPure.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using smatchet::AddedKeys;
using smatchet::RemovedKeys;

TEST_CASE("RemovedKeys: keys in cached but not fetched, in cached order") {
    const std::vector<std::string> cached = {"A-1", "A-2", "A-3", "A-4"};
    const std::vector<std::string> fetched = {"A-2", "A-4"};
    const std::vector<std::string> removed = RemovedKeys(cached, fetched);
    REQUIRE(removed.size() == 2);
    CHECK(removed[0] == "A-1");
    CHECK(removed[1] == "A-3");
}

TEST_CASE("RemovedKeys: nothing removed when fetched is a superset") {
    const std::vector<std::string> cached = {"A-1", "A-2"};
    const std::vector<std::string> fetched = {"A-1", "A-2", "A-9"};
    CHECK(RemovedKeys(cached, fetched).empty());
}

TEST_CASE("RemovedKeys: empty fetched removes everything (whole view vanished)") {
    const std::vector<std::string> cached = {"A-1", "A-2"};
    const std::vector<std::string> removed = RemovedKeys(cached, {});
    REQUIRE(removed.size() == 2);
    CHECK(removed[0] == "A-1");
    CHECK(removed[1] == "A-2");
}

TEST_CASE("RemovedKeys: empty cached yields nothing") { CHECK(RemovedKeys({}, {"A-1"}).empty()); }

TEST_CASE("RemovedKeys: duplicate cached key emitted at most once") {
    const std::vector<std::string> cached = {"A-1", "A-1", "A-2"};
    const std::vector<std::string> removed = RemovedKeys(cached, {});
    REQUIRE(removed.size() == 2);
    CHECK(removed[0] == "A-1");
    CHECK(removed[1] == "A-2");
}

TEST_CASE("AddedKeys: keys in fetched but not cached, in fetched order") {
    const std::vector<std::string> cached = {"A-1"};
    const std::vector<std::string> fetched = {"A-3", "A-1", "A-2"};
    const std::vector<std::string> added = AddedKeys(cached, fetched);
    REQUIRE(added.size() == 2);
    CHECK(added[0] == "A-3");
    CHECK(added[1] == "A-2");
}

TEST_CASE("AddedKeys: empty cached means the whole fetched set is new") {
    const std::vector<std::string> added = AddedKeys({}, {"A-1", "A-2"});
    REQUIRE(added.size() == 2);
    CHECK(added[0] == "A-1");
    CHECK(added[1] == "A-2");
}

TEST_CASE("AddedKeys: duplicate fetched key emitted at most once") {
    const std::vector<std::string> added = AddedKeys({}, {"A-1", "A-1", "A-2"});
    REQUIRE(added.size() == 2);
    CHECK(added[0] == "A-1");
    CHECK(added[1] == "A-2");
}

TEST_CASE("AddedKeys: no change yields nothing") {
    const std::vector<std::string> same = {"A-1", "A-2"};
    CHECK(AddedKeys(same, same).empty());
}
