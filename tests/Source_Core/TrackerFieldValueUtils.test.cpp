#include <doctest/doctest.h>

#include "TrackerFieldValueUtils.h"

#include <string>
#include <vector>

namespace {

TrackerField MakeFlatSelectField(const std::string& id) {
    TrackerField f;
    f.Id = id;
    TrackerFieldOption a;
    a.Id = "100";
    a.Value = "Alpha";
    TrackerFieldOption b;
    b.Id = "200";
    b.Value = "Beta";
    f.AllowedValueOptions = {a, b};
    return f;
}

TrackerField MakeCascadingField() {
    TrackerField f;
    f.Id = "cascade";
    TrackerFieldOption parent;
    parent.Id = "p1";
    parent.Value = "ParentA";
    TrackerFieldOption child;
    child.Id = "c1";
    child.Value = "ChildA1";
    parent.Children.push_back(child);
    f.AllowedValueOptions.push_back(parent);

    TrackerFieldOption parent2;
    parent2.Id = "p2";
    parent2.Value = "ParentB";
    f.AllowedValueOptions.push_back(parent2);
    return f;
}

} // namespace

TEST_CASE("TrackerFieldValueUtils::FindOptionRecursive finds by id or value at top level") {
    TrackerField f = MakeFlatSelectField("x");
    using TrackerFieldValueUtils::FindOptionRecursive;
    const TrackerFieldOption* byId = FindOptionRecursive(f.AllowedValueOptions, "100");
    REQUIRE(byId != nullptr);
    CHECK(byId->Value == "Alpha");

    const TrackerFieldOption* byValue = FindOptionRecursive(f.AllowedValueOptions, "Beta");
    REQUIRE(byValue != nullptr);
    CHECK(byValue->Id == "200");

    CHECK(FindOptionRecursive(f.AllowedValueOptions, "missing") == nullptr);
}

TEST_CASE("TrackerFieldValueUtils::FindOptionRecursive descends into children") {
    TrackerField f = MakeCascadingField();
    using TrackerFieldValueUtils::FindOptionRecursive;
    const TrackerFieldOption* found = FindOptionRecursive(f.AllowedValueOptions, "ChildA1");
    REQUIRE(found != nullptr);
    CHECK(found->Id == "c1");

    const TrackerFieldOption* foundById = FindOptionRecursive(f.AllowedValueOptions, "c1");
    REQUIRE(foundById != nullptr);
    CHECK(foundById->Value == "ChildA1");
}

TEST_CASE("TrackerFieldValueUtils::ResolveOptionId returns id when match, raw when miss") {
    TrackerField f = MakeFlatSelectField("x");
    CHECK(TrackerFieldValueUtils::ResolveOptionId(f, "Alpha") == "100");
    CHECK(TrackerFieldValueUtils::ResolveOptionId(f, "100") == "100");
    // Unmatched value passes through as-is so callers can still send raw text.
    CHECK(TrackerFieldValueUtils::ResolveOptionId(f, "unmatched") == "unmatched");
}

TEST_CASE("TrackerFieldValueUtils::ResolveOptionLabel returns Value when match, raw when miss") {
    TrackerField f = MakeFlatSelectField("x");
    CHECK(TrackerFieldValueUtils::ResolveOptionLabel(f, "100") == "Alpha");
    CHECK(TrackerFieldValueUtils::ResolveOptionLabel(f, "Beta") == "Beta");
    CHECK(TrackerFieldValueUtils::ResolveOptionLabel(f, "missing") == "missing");
}

TEST_CASE("TrackerFieldValueUtils::ResolveCurrentSelectionIds splits CSV and resolves each") {
    TrackerField f = MakeFlatSelectField("multi");
    std::vector<std::string> ids = TrackerFieldValueUtils::ResolveCurrentSelectionIds(f, "Alpha, Beta");
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "100");
    CHECK(ids[1] == "200");
}

TEST_CASE("TrackerFieldValueUtils::ResolveCurrentSelectionIds skips empty parts") {
    TrackerField f = MakeFlatSelectField("multi");
    std::vector<std::string> ids = TrackerFieldValueUtils::ResolveCurrentSelectionIds(f, "Alpha,, ,Beta");
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "100");
    CHECK(ids[1] == "200");
}

TEST_CASE("TrackerFieldValueUtils::EmptySelectPreviewLabel branches on IsUserType") {
    TrackerField selectField;
    selectField.IsUserType = false;
    CHECK(std::string(TrackerFieldValueUtils::EmptySelectPreviewLabel(selectField)) == "<none>");

    TrackerField userField;
    userField.IsUserType = true;
    CHECK(std::string(TrackerFieldValueUtils::EmptySelectPreviewLabel(userField)) == "Unassigned");
}

TEST_CASE("TrackerFieldValueUtils::TryResolveCascadingSelection handles unit-separator encoding") {
    TrackerField f = MakeCascadingField();
    std::string parent;
    std::string child;
    // Pre-encoded: "p1\x1fc1"
    std::string encoded;
    encoded.push_back('p');
    encoded.push_back('1');
    encoded.push_back('\x1f');
    encoded.push_back('c');
    encoded.push_back('1');
    CHECK(TrackerFieldValueUtils::TryResolveCascadingSelection(f, encoded, parent, child));
    CHECK(parent == "p1");
    CHECK(child == "c1");
}

TEST_CASE("TrackerFieldValueUtils::TryResolveCascadingSelection returns false for empty input") {
    TrackerField f = MakeCascadingField();
    std::string parent;
    std::string child;
    CHECK_FALSE(TrackerFieldValueUtils::TryResolveCascadingSelection(f, "", parent, child));
    CHECK(parent.empty());
    CHECK(child.empty());
}

TEST_CASE("TrackerFieldValueUtils::TryResolveCascadingSelection resolves a known child to (parent, child)") {
    TrackerField f = MakeCascadingField();
    std::string parent;
    std::string child;
    CHECK(TrackerFieldValueUtils::TryResolveCascadingSelection(f, "ChildA1", parent, child));
    CHECK(parent == "p1");
    CHECK(child == "c1");
}

TEST_CASE("TrackerFieldValueUtils::TryResolveCascadingSelection resolves a known parent to (parent, '')") {
    TrackerField f = MakeCascadingField();
    std::string parent;
    std::string child;
    CHECK(TrackerFieldValueUtils::TryResolveCascadingSelection(f, "ParentB", parent, child));
    CHECK(parent == "p2");
    CHECK(child.empty());
}

TEST_CASE("TrackerFieldValueUtils::TryResolveCascadingSelection passes unknown value as parent") {
    TrackerField f = MakeCascadingField();
    std::string parent;
    std::string child;
    CHECK(TrackerFieldValueUtils::TryResolveCascadingSelection(f, "mystery", parent, child));
    CHECK(parent == "mystery");
    CHECK(child.empty());
}

TEST_CASE("TrackerFieldValueUtils::IsTimeDurationField recognises Jira timetracking ids") {
    using TrackerFieldValueUtils::IsTimeDurationField;
    CHECK(IsTimeDurationField("timeoriginalestimate"));
    CHECK(IsTimeDurationField("timeestimate"));
    CHECK(IsTimeDurationField("timespent"));
    CHECK(IsTimeDurationField("aggregatetimeoriginalestimate"));
    CHECK(IsTimeDurationField("aggregatetimeestimate"));
    CHECK(IsTimeDurationField("aggregatetimespent"));
    CHECK_FALSE(IsTimeDurationField("summary"));
    CHECK_FALSE(IsTimeDurationField(""));
    CHECK_FALSE(IsTimeDurationField("TimeSpent")); // case-sensitive
}

TEST_CASE("TrackerFieldValueUtils::IsEditableTimetrackingEstimateFieldId distinguishes editable estimates") {
    using TrackerFieldValueUtils::IsEditableTimetrackingEstimateFieldId;
    CHECK(IsEditableTimetrackingEstimateFieldId("timeoriginalestimate"));
    CHECK(IsEditableTimetrackingEstimateFieldId("timeestimate"));
    CHECK_FALSE(IsEditableTimetrackingEstimateFieldId("timespent"));
    CHECK_FALSE(IsEditableTimetrackingEstimateFieldId("aggregatetimeestimate"));
    CHECK_FALSE(IsEditableTimetrackingEstimateFieldId(""));
}

TEST_CASE("TrackerFieldValueUtils::IsNonEditableTimetrackingFieldId flags aggregates + timespent") {
    using TrackerFieldValueUtils::IsNonEditableTimetrackingFieldId;
    CHECK(IsNonEditableTimetrackingFieldId("timespent"));
    CHECK(IsNonEditableTimetrackingFieldId("aggregatetimeoriginalestimate"));
    CHECK(IsNonEditableTimetrackingFieldId("aggregatetimeestimate"));
    CHECK(IsNonEditableTimetrackingFieldId("aggregatetimespent"));
    CHECK_FALSE(IsNonEditableTimetrackingFieldId("timeoriginalestimate"));
    CHECK_FALSE(IsNonEditableTimetrackingFieldId("timeestimate"));
    CHECK_FALSE(IsNonEditableTimetrackingFieldId("priority"));
}
