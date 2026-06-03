#include <doctest/doctest.h>

#include "SmatchetViewsDashboardUi_detail.h"

#include <string>
#include <vector>

// Bucket-A coverage for the pure (ImGui-free) helpers lifted out of
// SmatchetUI::drawViewsDashboardWindow during its function-size decomposition:
//   - IsBasicFieldId
//   - PrettyColumnLabel
//   - FieldSortLess
//   - CategorizeAvailableFields
// The header defines them all `inline`, so the doctest rig needs no extra .cpp
// link. Goldens captured from the real helper behaviour during decomposition.

using namespace SmatchetViewsDashboardUiDetail;

namespace {
TrackerField MakeField(const std::string& id, const std::string& name, bool isCustom) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.IsCustom = isCustom;
    return f;
}
} // namespace

TEST_CASE("IsBasicFieldId — exactly the six core ids") {
    CHECK(IsBasicFieldId("summary"));
    CHECK(IsBasicFieldId("assignee"));
    CHECK(IsBasicFieldId("priority"));
    CHECK(IsBasicFieldId("status"));
    CHECK(IsBasicFieldId("created"));
    CHECK(IsBasicFieldId("updated"));
    CHECK_FALSE(IsBasicFieldId("id"));
    CHECK_FALSE(IsBasicFieldId("description"));
    CHECK_FALSE(IsBasicFieldId(""));
    CHECK_FALSE(IsBasicFieldId("Summary")); // case-sensitive
}

TEST_CASE("PrettyColumnLabel — id, field:<id>, and passthrough") {
    std::vector<TrackerField> fields;
    fields.push_back(MakeField("summary", "Summary", false));
    fields.push_back(MakeField("customfield_1", "Story Points", true));

    CHECK(PrettyColumnLabel("id", fields) == "ID");
    CHECK(PrettyColumnLabel("field:summary", fields) == "Summary (summary)");
    CHECK(PrettyColumnLabel("field:customfield_1", fields) == "Story Points (customfield_1)");
    // Unknown field id falls back to the bare id.
    CHECK(PrettyColumnLabel("field:missing", fields) == "missing");
    // Any other key passes through unchanged.
    CHECK(PrettyColumnLabel("weird_key", fields) == "weird_key");
}

TEST_CASE("FieldSortLess — by Name then Id, null sorts last") {
    TrackerField a = MakeField("b", "Alpha", false);
    TrackerField b = MakeField("a", "Beta", false);
    TrackerField c = MakeField("a", "Alpha", false); // same Name as a, smaller Id

    CHECK(FieldSortLess(&a, &b));       // "Alpha" < "Beta"
    CHECK_FALSE(FieldSortLess(&b, &a)); // "Beta" not < "Alpha"
    CHECK(FieldSortLess(&c, &a));       // same Name, Id "a" < "b"
    CHECK_FALSE(FieldSortLess(&a, &c));
    // Null pointer sorts last: non-null < null is true; null < non-null is false.
    CHECK(FieldSortLess(&a, nullptr));
    CHECK_FALSE(FieldSortLess(nullptr, &a));
    CHECK_FALSE(FieldSortLess(nullptr, nullptr));
}

TEST_CASE("CategorizeAvailableFields — partition into visible/system/custom/basic, search-filtered") {
    std::vector<TrackerField> fields;
    fields.push_back(MakeField("status", "Status", false));       // basic
    fields.push_back(MakeField("summary", "Summary", false));     // basic
    fields.push_back(MakeField("reporter", "Reporter", false));   // system
    fields.push_back(MakeField("labels", "Labels", false));       // system
    fields.push_back(MakeField("customfield_9", "Sprint", true)); // custom
    fields.push_back(MakeField("customfield_2", "Epic", true));   // custom

    SUBCASE("empty needle keeps all, groups sorted") {
        const CategorizedFields cat = CategorizeAvailableFields(fields, "");
        CHECK(cat.visible.size() == 6);
        CHECK(cat.basic.size() == 2);
        CHECK(cat.system.size() == 2);
        CHECK(cat.custom.size() == 2);
        // System sorted by Name: "Labels" < "Reporter".
        REQUIRE(cat.system.size() == 2);
        CHECK(cat.system[0]->Id == "labels");
        CHECK(cat.system[1]->Id == "reporter");
        // Custom sorted by Name: "Epic" < "Sprint".
        REQUIRE(cat.custom.size() == 2);
        CHECK(cat.custom[0]->Id == "customfield_2");
        CHECK(cat.custom[1]->Id == "customfield_9");
        // Basic keeps catalog order (status before summary).
        REQUIRE(cat.basic.size() == 2);
        CHECK(cat.basic[0]->Id == "status");
        CHECK(cat.basic[1]->Id == "summary");
        // Visible keeps catalog order.
        CHECK(cat.visible[0]->Id == "status");
        CHECK(cat.visible[5]->Id == "customfield_2");
    }

    SUBCASE("needle filters by Id or Name, case-insensitive") {
        const CategorizedFields cat = CategorizeAvailableFields(fields, "sum");
        CHECK(cat.visible.size() == 1);
        CHECK(cat.basic.size() == 1);
        CHECK(cat.basic[0]->Id == "summary");
        CHECK(cat.system.empty());
        CHECK(cat.custom.empty());

        const CategorizedFields byName = CategorizeAvailableFields(fields, "EPIC");
        CHECK(byName.visible.size() == 1);
        CHECK(byName.custom.size() == 1);
        CHECK(byName.custom[0]->Id == "customfield_2");
    }
}
