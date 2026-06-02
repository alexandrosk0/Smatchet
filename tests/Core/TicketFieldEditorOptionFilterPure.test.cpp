#include <doctest/doctest.h>

#include "TicketFieldEditorOptionFilterPure.h"

#include <string>

using TicketFieldEditorOptionFilterPure::EffectiveOptionId;
using TicketFieldEditorOptionFilterPure::OptionMatchesFilter;

namespace {
TrackerFieldOption MakeOption(const std::string& id, const std::string& value, const std::string& secondary = "") {
    TrackerFieldOption o;
    o.Id = id;
    o.Value = value;
    o.SecondaryValue = secondary;
    return o;
}
} // namespace

TEST_CASE("EffectiveOptionId: prefers Id, falls back to Value") {
    CHECK(EffectiveOptionId(MakeOption("10001", "High")) == "10001");
    CHECK(EffectiveOptionId(MakeOption("", "High")) == "High");
    CHECK(EffectiveOptionId(MakeOption("", "")) == "");
}

TEST_CASE("OptionMatchesFilter: empty filter matches every option") {
    CHECK(OptionMatchesFilter(MakeOption("1", "Bug"), ""));
    CHECK(OptionMatchesFilter(MakeOption("", ""), ""));
}

TEST_CASE("OptionMatchesFilter: matches against Value (filter pre-lowercased)") {
    const TrackerFieldOption opt = MakeOption("10", "In Progress");
    // Caller lower-cases the filter; the predicate lower-cases the option fields.
    CHECK(OptionMatchesFilter(opt, "progress"));
    CHECK(OptionMatchesFilter(opt, "in pro"));
    CHECK_FALSE(OptionMatchesFilter(opt, "done"));
}

TEST_CASE("OptionMatchesFilter: matches against SecondaryValue") {
    const TrackerFieldOption opt = MakeOption("10", "Alice", "alice@example.com");
    CHECK(OptionMatchesFilter(opt, "example.com"));
    CHECK(OptionMatchesFilter(opt, "alice@"));
    CHECK_FALSE(OptionMatchesFilter(opt, "bob"));
}

TEST_CASE("OptionMatchesFilter: matches against effective id") {
    SUBCASE("explicit Id is searched") {
        const TrackerFieldOption opt = MakeOption("customfield_42", "Widget");
        CHECK(OptionMatchesFilter(opt, "customfield_42"));
        CHECK(OptionMatchesFilter(opt, "field_4"));
    }
    SUBCASE("when Id empty, Value is the effective id and is searched") {
        const TrackerFieldOption opt = MakeOption("", "fallbackid");
        CHECK(OptionMatchesFilter(opt, "fallbackid"));
    }
}

TEST_CASE("OptionMatchesFilter: no match across any field returns false") {
    const TrackerFieldOption opt = MakeOption("99", "Critical", "sev-1");
    CHECK_FALSE(OptionMatchesFilter(opt, "zzz"));
}
