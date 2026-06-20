// Bucket-A coverage for MembershipDiffPure (ticket-change-monitor plan, S1a) — the pure
// set-difference the per-pane change monitor uses to reconcile its tracked set against the
// view's freshly-fetched key list. RemovedKeys -> candidates for the left-view/deleted probe;
// AddedKeys -> keys that newly entered the view. ClassifyMembershipRemovals -> the apply-side
// split of post-probe verdicts into pane-resident removals vs shared-cache deletes (S1c-3,
// item 10). Header-only (no production .cpp to link).

#include "Sync/MembershipDiffPure.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using smatchet::AddedKeys;
using smatchet::ClassifyMembershipRemovals;
using smatchet::MembershipReconcilePlan;
using smatchet::MembershipRemovalVerdict;
using smatchet::RemovedKeys;

namespace {

MembershipRemovalVerdict Verdict(const std::string& key, bool stillExists) {
    MembershipRemovalVerdict v;
    v.issueKey = key;
    v.stillExists = stillExists;
    return v;
}

} // namespace

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

TEST_CASE("ClassifyMembershipRemovals: resident + still-exists -> LeftView removal, no cache delete") {
    const std::vector<std::string> resident = {"A-1", "A-2"};
    const std::vector<MembershipRemovalVerdict> verdicts = {Verdict("A-1", true)};
    const MembershipReconcilePlan plan = ClassifyMembershipRemovals(resident, verdicts);
    REQUIRE(plan.residentRemovals.size() == 1);
    CHECK(plan.residentRemovals[0].issueKey == "A-1");
    CHECK(plan.residentRemovals[0].stillExists == true);
    CHECK(plan.cacheDeleteKeys.empty());
}

TEST_CASE("ClassifyMembershipRemovals: resident + gone -> Deleted removal AND cache delete") {
    const std::vector<std::string> resident = {"A-1", "A-2"};
    const std::vector<MembershipRemovalVerdict> verdicts = {Verdict("A-2", false)};
    const MembershipReconcilePlan plan = ClassifyMembershipRemovals(resident, verdicts);
    REQUIRE(plan.residentRemovals.size() == 1);
    CHECK(plan.residentRemovals[0].issueKey == "A-2");
    CHECK(plan.residentRemovals[0].stillExists == false);
    REQUIRE(plan.cacheDeleteKeys.size() == 1);
    CHECK(plan.cacheDeleteKeys[0] == "A-2");
}

TEST_CASE("ClassifyMembershipRemovals: non-resident + gone -> cache delete only, no pane removal") {
    const std::vector<std::string> resident = {"A-1"};
    const std::vector<MembershipRemovalVerdict> verdicts = {Verdict("Z-9", false)};
    const MembershipReconcilePlan plan = ClassifyMembershipRemovals(resident, verdicts);
    CHECK(plan.residentRemovals.empty());
    REQUIRE(plan.cacheDeleteKeys.size() == 1);
    CHECK(plan.cacheDeleteKeys[0] == "Z-9");
}

TEST_CASE("ClassifyMembershipRemovals: non-resident + still-exists -> pure no-op") {
    const std::vector<std::string> resident = {"A-1"};
    const std::vector<MembershipRemovalVerdict> verdicts = {Verdict("Z-9", true)};
    const MembershipReconcilePlan plan = ClassifyMembershipRemovals(resident, verdicts);
    CHECK(plan.residentRemovals.empty());
    CHECK(plan.cacheDeleteKeys.empty());
}

TEST_CASE("ClassifyMembershipRemovals: input order preserved across both output lists") {
    const std::vector<std::string> resident = {"A-1", "A-2", "A-3"};
    const std::vector<MembershipRemovalVerdict> verdicts = {Verdict("A-3", false), Verdict("A-1", true),
                                                            Verdict("A-2", false)};
    const MembershipReconcilePlan plan = ClassifyMembershipRemovals(resident, verdicts);
    REQUIRE(plan.residentRemovals.size() == 3);
    CHECK(plan.residentRemovals[0].issueKey == "A-3");
    CHECK(plan.residentRemovals[1].issueKey == "A-1");
    CHECK(plan.residentRemovals[2].issueKey == "A-2");
    REQUIRE(plan.cacheDeleteKeys.size() == 2);
    CHECK(plan.cacheDeleteKeys[0] == "A-3");
    CHECK(plan.cacheDeleteKeys[1] == "A-2");
}

TEST_CASE("ClassifyMembershipRemovals: empty verdicts -> empty plan") {
    const MembershipReconcilePlan plan = ClassifyMembershipRemovals({"A-1", "A-2"}, {});
    CHECK(plan.residentRemovals.empty());
    CHECK(plan.cacheDeleteKeys.empty());
}
