#include "SmatchetNewIssueDraftUi_detail.h"

#include <doctest/doctest.h>

using SmatchetNewIssueDraft::detail::FieldUsesOptionDropdown;
using SmatchetNewIssueDraft::detail::OptionMatchesComboFilter;

namespace {
TrackerFieldOption MakeOption(const std::string& id, const std::string& value, const std::string& secondary) {
    TrackerFieldOption o;
    o.Id = id;
    o.Value = value;
    o.SecondaryValue = secondary;
    return o;
}
} // namespace

TEST_CASE("OptionMatchesComboFilter: empty filter matches every option") {
    const TrackerFieldOption o = MakeOption("10001", "Bug", "");
    CHECK(OptionMatchesComboFilter(o, ""));
}

TEST_CASE("OptionMatchesComboFilter: case-insensitive match on the display value") {
    const TrackerFieldOption o = MakeOption("10001", "Bug", "");
    CHECK(OptionMatchesComboFilter(o, "bug"));
    CHECK(OptionMatchesComboFilter(o, "bu"));
    CHECK_FALSE(OptionMatchesComboFilter(o, "task"));
}

TEST_CASE("OptionMatchesComboFilter: matches secondary value and option id") {
    const TrackerFieldOption o = MakeOption("acct-42", "Alice", "alice@example.com");
    CHECK(OptionMatchesComboFilter(o, "example.com")); // secondary value
    CHECK(OptionMatchesComboFilter(o, "acct"));        // option id
    CHECK(OptionMatchesComboFilter(o, "alice"));       // display value
}

TEST_CASE("OptionMatchesComboFilter: empty id falls back to the value for the id-match test") {
    const TrackerFieldOption o = MakeOption("", "InProgress", "");
    // optId resolves to Value when Id is empty, so a value substring still matches.
    CHECK(OptionMatchesComboFilter(o, "progress"));
}

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
