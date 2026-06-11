#include <doctest/doctest.h>

#include "SmatchetViewsDashboardUi_detail.h"

#include <string>
#include <unordered_set>
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

// ---------------------------------------------------------------------------
// #views-field-uncheck — the selected-field set is now the source of truth. The
// Views Fields tab could not add a field once the sorted-CSV of the selection
// exceeded 1023 bytes: the set was re-derived every frame by PARSING a fixed
// 1024-byte char buffer, and CopyStringToBuffer silently truncated mid-token,
// so any field past the cutoff was dropped on the round-trip and auto-unchecked.
// These cases pin the pure serialization seam that now backs the authoritative
// set: a large selection must survive set -> serialize -> deserialize with NO
// loss, and CopyStringToBuffer must REPORT truncation rather than swallow it.
// ---------------------------------------------------------------------------

TEST_CASE("SerializeSelectedFields/DeserializeSelectedFields — 100-field selection round-trips losslessly") {
    std::unordered_set<std::string> selected;
    for (int i = 0; i < 100; ++i) {
        selected.insert("customfield_" + std::to_string(10000 + i));
    }
    // Long-id field that sorts late — the exact casualty class of the bug
    // (timeoriginalestimate landed past the 1023-byte cutoff under a full catalog).
    selected.insert("timeoriginalestimate");

    const std::string csv = SerializeSelectedFields(selected);
    // The serialized form is comfortably past the old 1024-byte buffer cap.
    CHECK(csv.size() > 1023);

    const std::unordered_set<std::string> restored = DeserializeSelectedFields(csv);
    CHECK(restored.size() == selected.size());
    CHECK(restored == selected);
    // The late-sorting field specifically survives (the consistent casualty).
    CHECK(restored.count("timeoriginalestimate") == 1);
}

TEST_CASE("SerializeSelectedFields — canonical sorted order, deterministic CSV") {
    std::unordered_set<std::string> selected = {"status", "assignee", "summary"};
    CHECK(SerializeSelectedFields(selected) == "assignee, status, summary");

    std::unordered_set<std::string> empty;
    CHECK(SerializeSelectedFields(empty).empty());
    CHECK(DeserializeSelectedFields("").empty());
}

TEST_CASE("CopyStringToBuffer — reports fit vs truncation (#views-field-uncheck loud-truncation guard)") {
    char small[8] = {};
    // Fits exactly at cap (N-1 == 7 bytes).
    CHECK(CopyStringToBuffer(small, "1234567"));
    CHECK(std::string(small) == "1234567");
    // One byte over cap -> truncated, returns false, clipped to 7 bytes.
    CHECK_FALSE(CopyStringToBuffer(small, "12345678"));
    CHECK(std::string(small) == "1234567");

    // A >1023-byte selection CSV truncates in the legacy 1024-byte buffer — the
    // exact silent-drop the set-authority fix removes from the round-trip path.
    char legacyBuf[1024] = {};
    std::unordered_set<std::string> selected;
    for (int i = 0; i < 100; ++i) {
        selected.insert("customfield_" + std::to_string(10000 + i));
    }
    const std::string csv = SerializeSelectedFields(selected);
    REQUIRE(csv.size() > 1023);
    CHECK_FALSE(CopyStringToBuffer(legacyBuf, csv));
    // Parsing the truncated buffer back loses fields — proving why the buffer can
    // no longer be the authority (the set is).
    const std::unordered_set<std::string> fromTruncated = DeserializeSelectedFields(legacyBuf);
    CHECK(fromTruncated.size() < selected.size());
}
