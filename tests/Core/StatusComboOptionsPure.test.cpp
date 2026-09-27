// StatusComboOptionsPure bucket-A tests — which options the status combo offers (Quality Pillar 6):
// live transitions, else the remembered workflow, else every catalog status; the current status is
// always listed so the combo never opens empty. Pure, header-only.

#include "StatusComboOptionsPure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

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
