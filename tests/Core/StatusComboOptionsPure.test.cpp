// StatusComboOptionsPure bucket-A tests — which options the status combo offers (Quality Pillar 6):
// live transitions, else the remembered workflow, else every catalog status; the current status is
// always listed so the combo never opens empty. Pure, header-only.

#include "StatusComboOptionsPure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::statuscombo::IntersectStatusOptions;
using smatchet::statuscombo::LessLiveSource;
using smatchet::statuscombo::PickStatusComboOptions;
using smatchet::statuscombo::StatusComboPick;
using smatchet::statuscombo::StatusOptionsSource;

namespace {

TrackerFieldOption Option(const std::string& id, const std::string& value) {
    TrackerFieldOption opt;
    opt.Id = id;
    opt.Value = value;
    return opt;
}

std::vector<TrackerFieldOption> Catalog() {
    return {Option("1", "To Do"), Option("2", "In Progress"), Option("3", "Done"), Option("4", "Blocked")};
}

} // namespace

TEST_CASE("StatusComboOptionsPure — live targets come first after the current status") {
    const StatusComboPick pick = PickStatusComboOptions({Option("2", "In Progress"), Option("3", "Done")}, true,
                                                        Catalog(), Option("1", "To Do"));
    CHECK(pick.From == StatusOptionsSource::Live);
    REQUIRE(pick.Options.size() == 3);
    CHECK(pick.Options[0].Id == "1");
    CHECK(pick.Options[1].Id == "2");
    CHECK(pick.Options[2].Id == "3");
}

TEST_CASE("StatusComboOptionsPure — learned targets are used and marked Learned") {
    const StatusComboPick pick =
        PickStatusComboOptions({Option("2", "In Progress")}, false, Catalog(), Option("1", "To Do"));
    CHECK(pick.From == StatusOptionsSource::Learned);
    REQUIRE(pick.Options.size() == 2);
    CHECK(pick.Options[0].Id == "1");
    CHECK(pick.Options[1].Id == "2");
}

TEST_CASE("StatusComboOptionsPure — no remembered targets falls back to every catalog status") {
    const StatusComboPick pick = PickStatusComboOptions({}, false, Catalog(), Option("1", "To Do"));
    CHECK(pick.From == StatusOptionsSource::Catalog);
    REQUIRE(pick.Options.size() == 4); // current is already in the catalog: no duplicate
    CHECK(pick.Options[0].Id == "1");
    CHECK(pick.Options[3].Id == "4");
}

TEST_CASE("StatusComboOptionsPure — an empty live set is authoritative: only the current status") {
    const StatusComboPick pick = PickStatusComboOptions({}, true, Catalog(), Option("1", "To Do"));
    CHECK(pick.From == StatusOptionsSource::Live);
    REQUIRE(pick.Options.size() == 1);
    CHECK(pick.Options[0].Id == "1");
}

TEST_CASE("StatusComboOptionsPure — a current status already listed is not duplicated") {
    const StatusComboPick pick = PickStatusComboOptions({Option("1", "To Do"), Option("2", "In Progress")}, true,
                                                        Catalog(), Option("1", "To Do"));
    CHECK(pick.Options.size() == 2);
}

TEST_CASE("StatusComboOptionsPure — never empty when the current status is known") {
    const StatusComboPick pick = PickStatusComboOptions({}, false, {}, Option("9", "Custom"));
    CHECK(pick.From == StatusOptionsSource::Catalog);
    REQUIRE(pick.Options.size() == 1);
    CHECK(pick.Options[0].Value == "Custom");
}

TEST_CASE("StatusComboOptionsPure — an id-less option matches the current status by value") {
    const StatusComboPick pick =
        PickStatusComboOptions({Option("", "To Do"), Option("", "Done")}, false, Catalog(), Option("", "To Do"));
    REQUIRE(pick.Options.size() == 2);
    CHECK(pick.Options[0].Value == "To Do");
}

TEST_CASE("StatusComboOptionsPure — a current status with neither id nor value is not added") {
    const StatusComboPick pick = PickStatusComboOptions({}, false, {}, Option("", ""));
    CHECK(pick.Options.empty());
}

TEST_CASE("StatusComboOptionsPure — two live transitions to one status name list it once, with a live id") {
    const StatusComboPick pick =
        PickStatusComboOptions({Option("20", "In Progress"), Option("21", "Done"), Option("30", "In Progress")}, true,
                               {}, Option("1", "To Do"));
    REQUIRE(pick.Options.size() == 3); // the current status, then the two distinct targets
    CHECK(pick.Options[1].Id == "20"); // the first target's id: valid for this issue
    CHECK(pick.Options[1].Value == "In Progress");
    CHECK(pick.Options[2].Id == "21");
}

TEST_CASE("StatusComboOptionsPure — a learned list also shows each name once; the catalog is used as merged") {
    const std::vector<TrackerFieldOption> sameName = {Option("20", "In Progress"), Option("30", "IN PROGRESS")};
    const StatusComboPick learned = PickStatusComboOptions(sameName, false, Catalog(), Option("1", "To Do"));
    CHECK(learned.From == StatusOptionsSource::Learned);
    REQUIRE(learned.Options.size() == 2);
    CHECK(learned.Options[1].Value == "In Progress"); // the first spelling stays

    // The catalog's status list is merged where it is built and loaded (TrackerFieldCatalogPure,
    // FieldCatalogCache), so the combo takes it as is and only adds the current status.
    std::vector<TrackerFieldOption> merged = sameName;
    smatchet::statusoptions::MergeSameNameStatusOptions(merged);
    const StatusComboPick catalog = PickStatusComboOptions({}, false, merged, Option("1", "To Do"));
    CHECK(catalog.From == StatusOptionsSource::Catalog);
    REQUIRE(catalog.Options.size() == 2);
    CHECK(catalog.Options[1].HasId("30"));
}

TEST_CASE("StatusComboOptionsPure — a target named like the current status is the current row, not a second one") {
    // The issue's workflow lists its own "In Progress" through a global transition; the catalog resolves
    // the current status to another workflow's id. One row, carrying the id the combo treats as current,
    // so re-picking it is ignored and nothing is written.
    const StatusComboPick pick = PickStatusComboOptions({Option("20", "in progress"), Option("21", "Done")}, true, {},
                                                        Option("2", "In Progress"));
    REQUIRE(pick.Options.size() == 2);
    CHECK(pick.Options[0].Id == "2");
    CHECK(pick.Options[1].Id == "21");
}

// The bulk-edit status picker offers what every selected row can move to: each row's own list (its current status
// stays in it), intersected.
TEST_CASE("StatusComboOptionsPure — intersect keeps the statuses every row offers, in the first row's order") {
    const std::vector<std::vector<TrackerFieldOption>> lists = {
        {Option("1", "To Do"), Option("2", "In Progress"), Option("3", "Done")},
        {Option("2", "In Progress"), Option("3", "Done"), Option("4", "Blocked")},
        {Option("3", "Done"), Option("2", "In Progress")}};
    const std::vector<TrackerFieldOption> common = IntersectStatusOptions(lists);
    REQUIRE(common.size() == 2);
    CHECK(common[0].Id == "2");
    CHECK(common[1].Id == "3");
}

TEST_CASE("StatusComboOptionsPure — intersect is empty when no status is shared, or there are no rows") {
    CHECK(IntersectStatusOptions({}).empty());
    CHECK(IntersectStatusOptions({{Option("1", "To Do")}, {Option("2", "Done")}}).empty());
}

TEST_CASE("StatusComboOptionsPure — intersect of one row is that row's list") {
    const std::vector<TrackerFieldOption> only = {Option("1", "To Do"), Option("2", "Done")};
    CHECK(IntersectStatusOptions({only}).size() == 2);
}

TEST_CASE("StatusComboOptionsPure — intersect matches by name when the ids differ between workflows") {
    // One status name carries a different id in each project's workflow.
    const std::vector<TrackerFieldOption> common =
        IntersectStatusOptions({{Option("11", "Done")}, {Option("21", "Done")}, {Option("", "done")}});
    REQUIRE(common.size() == 1);
    CHECK(common[0].Id == "11"); // the first row's option
}

TEST_CASE("StatusComboOptionsPure — a row already at a status does not rule it out") {
    // Row A is at To Do and may move to Done; row B is at Done (its own list keeps Done as the current status).
    const StatusComboPick rowA = PickStatusComboOptions({Option("3", "Done")}, true, Catalog(), Option("1", "To Do"));
    const StatusComboPick rowB = PickStatusComboOptions({Option("1", "To Do")}, true, Catalog(), Option("3", "Done"));
    const std::vector<TrackerFieldOption> common = IntersectStatusOptions({rowA.Options, rowB.Options});
    // Done is common (A moves to it, B is there); To Do is common too (A is there, B moves to it).
    REQUIRE(common.size() == 2);
}

TEST_CASE("StatusComboOptionsPure — the staler freshness wins") {
    using smatchet::offline::DataFreshness;
    using smatchet::statuscombo::LeastFresh;
    CHECK(LeastFresh(DataFreshness::Fresh, DataFreshness::Refreshing) == DataFreshness::Refreshing);
    CHECK(LeastFresh(DataFreshness::CachedStale, DataFreshness::CachedOffline) == DataFreshness::CachedStale);
    CHECK(LeastFresh(DataFreshness::LoadingNoCache, DataFreshness::UnavailableNoCache) ==
          DataFreshness::UnavailableNoCache);
    CHECK(LeastFresh(DataFreshness::Fresh, DataFreshness::Fresh) == DataFreshness::Fresh);
}

TEST_CASE("StatusComboOptionsPure — the less live source wins") {
    CHECK(LessLiveSource(StatusOptionsSource::Live, StatusOptionsSource::Learned) == StatusOptionsSource::Learned);
    CHECK(LessLiveSource(StatusOptionsSource::Catalog, StatusOptionsSource::Live) == StatusOptionsSource::Catalog);
    CHECK(LessLiveSource(StatusOptionsSource::Live, StatusOptionsSource::Live) == StatusOptionsSource::Live);
}
