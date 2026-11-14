#include "SmatchetNewIssueDraftUi_detail.h"

#include <doctest/doctest.h>

using SmatchetNewIssueDraft::detail::FieldUsesOptionDropdown;

namespace {
TrackerFieldOption MakeOption(const std::string& id, const std::string& value, const std::string& secondary) {
    TrackerFieldOption o;
    o.Id = id;
    o.Value = value;
    o.SecondaryValue = secondary;
    return o;
}
} // namespace

TEST_CASE("FieldUsesOptionDropdown: issuetype always uses the dropdown, even with a null field") {
    CHECK(FieldUsesOptionDropdown("issuetype", nullptr));
}

TEST_CASE("FieldUsesOptionDropdown: a null non-issuetype field never uses the dropdown") {
    CHECK_FALSE(FieldUsesOptionDropdown("summary", nullptr));
}

TEST_CASE("FieldUsesOptionDropdown: single-valued select with options uses the dropdown") {
    TrackerField f;
    f.Id = "priority";
    f.Family = TrackerFieldFamily::SelectSingle;
    f.AllowedValueOptions.push_back(MakeOption("1", "High", ""));
    CHECK(FieldUsesOptionDropdown("priority", &f));
}

TEST_CASE("FieldUsesOptionDropdown: an array field is excluded even when it carries options") {
    TrackerField f;
    f.Id = "labels";
    f.IsArray = true;
    f.Family = TrackerFieldFamily::SelectSingle;
    f.AllowedValueOptions.push_back(MakeOption("1", "x", ""));
    CHECK_FALSE(FieldUsesOptionDropdown("labels", &f));
}

TEST_CASE("FieldUsesOptionDropdown: a select field with no options is excluded") {
    TrackerField f;
    f.Id = "component";
    f.Family = TrackerFieldFamily::SelectSingle;
    CHECK_FALSE(FieldUsesOptionDropdown("component", &f));
}

TEST_CASE("FieldUsesOptionDropdown: a user-type field with options uses the dropdown") {
    TrackerField f;
    f.Id = "assignee";
    f.IsUserType = true;
    f.AllowedValueOptions.push_back(MakeOption("acct-1", "Alice", ""));
    CHECK(FieldUsesOptionDropdown("assignee", &f));
}
