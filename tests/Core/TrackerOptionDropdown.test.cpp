#include <doctest/doctest.h>

#include "TrackerOptionDropdown.h"

#include <string>

using smatchet::dropdown::Item;
using smatchet::dropdown::ItemForOption;
using smatchet::dropdown::OptionPickId;

TEST_CASE("OptionPickId: the Id, else the Value, never empty for a named option") {
    TrackerFieldOption withId;
    withId.Id = "10001";
    withId.Value = "High";
    CHECK(std::string(OptionPickId(withId)) == "10001");

    // An option the tracker returned without an id is picked by its name. Picking it as "" would be
    // read as a field clear.
    TrackerFieldOption nameOnly;
    nameOnly.Value = "Backlog";
    CHECK(std::string(OptionPickId(nameOnly)) == "Backlog");
}

TEST_CASE("ItemForOption: id, label and secondary text come from the option") {
    TrackerFieldOption option;
    option.Id = "acc-1";
    option.Value = "Alice Smith";
    option.SecondaryValue = "alice@example.com";
    const Item item = ItemForOption(option);
    CHECK(std::string(item.Id) == "acc-1");
    CHECK(std::string(item.Label) == "Alice Smith");
    REQUIRE(item.Secondary != nullptr);
    CHECK(std::string(item.Secondary) == "alice@example.com");
    CHECK(item.Tooltip == nullptr);
    CHECK_FALSE(item.Checked);
    CHECK_FALSE(item.Disabled);

    TrackerFieldOption plain;
    plain.Value = "Bug";
    const Item plainItem = ItemForOption(plain);
    CHECK(std::string(plainItem.Id) == "Bug");
    CHECK(plainItem.Secondary == nullptr); // no empty dimmed text beside the label
}
