// FieldOptionsJsonPure — the TrackerFieldOption JSON codec shared by the field-catalog snapshot and the
// offline component rows (Quality Pillar 6). Pure: no I/O.

#include "FieldOptionsJsonPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

TrackerFieldOption FullOption() {
    TrackerFieldOption option;
    option.Id = "10";
    option.Value = "Parent";
    option.SecondaryValue = "Parent description";
    option.PayloadJson = R"({"id":"10","value":"Parent"})";
    option.Disabled = true;
    TrackerFieldOption child;
    child.Id = "11";
    child.Value = "Child";
    option.Children.push_back(child);
    return option;
}

} // namespace

TEST_SUITE("FieldOptionsJsonPure") {

    TEST_CASE("a serialize -> parse round trip keeps every member, children included") {
        const std::string json = smatchet::fieldoptions::SerializeFieldOptions({FullOption()});

        std::vector<TrackerFieldOption> parsed;
        REQUIRE(smatchet::fieldoptions::ParseFieldOptions(json, parsed));
        REQUIRE(parsed.size() == 1u);
        CHECK(parsed[0].Id == "10");
        CHECK(parsed[0].Value == "Parent");
        CHECK(parsed[0].SecondaryValue == "Parent description");
        CHECK(parsed[0].PayloadJson == R"({"id":"10","value":"Parent"})");
        CHECK(parsed[0].Disabled);
        REQUIRE(parsed[0].Children.size() == 1u);
        CHECK(parsed[0].Children[0].Value == "Child");
    }

    TEST_CASE("an empty list round-trips to an empty list") {
        std::vector<TrackerFieldOption> parsed{FullOption()};
        REQUIRE(smatchet::fieldoptions::ParseFieldOptions(smatchet::fieldoptions::SerializeFieldOptions({}), parsed));
        CHECK(parsed.empty());
    }

    TEST_CASE("unreadable rows are rejected without throwing") {
        std::vector<TrackerFieldOption> parsed{FullOption()};
        CHECK_FALSE(smatchet::fieldoptions::ParseFieldOptions("not json", parsed));
        CHECK(parsed.empty());
        CHECK_FALSE(smatchet::fieldoptions::ParseFieldOptions(R"({"id":"1"})", parsed)); // not an array
        // A mistyped member makes the whole row unreadable (never a half-parsed list).
        CHECK_FALSE(smatchet::fieldoptions::ParseFieldOptions(R"([{"id":"1"},{"id":7}])", parsed));
        CHECK(parsed.empty());
        // A depth bomb is refused by the bounded parse instead of overflowing the stack.
        CHECK_FALSE(
            smatchet::fieldoptions::ParseFieldOptions(std::string(100000, '[') + std::string(100000, ']'), parsed));
    }

    TEST_CASE("non-object entries are skipped and missing members default") {
        std::vector<TrackerFieldOption> parsed;
        REQUIRE(smatchet::fieldoptions::ParseFieldOptions(R"([1, "x", {"value":"Only a name"}])", parsed));
        REQUIRE(parsed.size() == 1u);
        CHECK(parsed[0].Id.empty());
        CHECK(parsed[0].Value == "Only a name");
        CHECK_FALSE(parsed[0].Disabled);
    }

    TEST_CASE("children nested past the depth cap are dropped, not recursed into") {
        TrackerFieldOption root;
        root.Id = "0";
        TrackerFieldOption* node = &root;
        for (int depth = 1; depth <= smatchet::fieldoptions::kMaxOptionDepth + 5; ++depth) {
            TrackerFieldOption child;
            child.Id = std::to_string(depth);
            node->Children.push_back(child);
            node = &node->Children.back();
        }

        std::vector<TrackerFieldOption> parsed;
        REQUIRE(
            smatchet::fieldoptions::ParseFieldOptions(smatchet::fieldoptions::SerializeFieldOptions({root}), parsed));
        int levels = 0;
        const TrackerFieldOption* walk = &parsed[0];
        while (!walk->Children.empty()) {
            walk = &walk->Children[0];
            ++levels;
        }
        CHECK(levels == smatchet::fieldoptions::kMaxOptionDepth);
    }
}
