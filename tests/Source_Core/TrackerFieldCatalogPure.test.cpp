#include <doctest/doctest.h>

#include "TrackerFieldCatalogPure.h"

#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using nlohmann::json;
using TrackerFieldCatalogPure::ComponentJsonIdToString;
using TrackerFieldCatalogPure::ExtractComponentOption;
using TrackerFieldCatalogPure::MergeComponentIntoCatalog;
using TrackerFieldCatalogPure::ResolveComponentJsonBean;
using TrackerFieldCatalogPure::SortComponentCatalog;

namespace {

TrackerField MakeComponentsField() {
    TrackerField f;
    f.Id = "components";
    f.Name = "Components";
    f.Type = "array";
    f.IsArray = true;
    f.ItemsType = "component";
    return f;
}

const TrackerFieldOption* FindOption(const std::vector<TrackerFieldOption>& opts, const std::string& id) {
    auto it = std::find_if(opts.begin(), opts.end(),
                           [&](const TrackerFieldOption& o) { return o.Id == id; });
    return it == opts.end() ? nullptr : &(*it);
}

} // namespace

TEST_CASE("ComponentJsonIdToString accepts string / signed / unsigned ints; rejects others") {
    SUBCASE("string passthrough") {
        CHECK(ComponentJsonIdToString(json("10001")) == "10001");
        CHECK(ComponentJsonIdToString(json("abc")) == "abc");
        CHECK(ComponentJsonIdToString(json("")) == "");
    }
    SUBCASE("signed integer to decimal") {
        CHECK(ComponentJsonIdToString(json(0)) == "0");
        CHECK(ComponentJsonIdToString(json(42)) == "42");
        CHECK(ComponentJsonIdToString(json(-7)) == "-7");
    }
    SUBCASE("unsigned integer to decimal") {
        CHECK(ComponentJsonIdToString(json(123456789u)) == "123456789");
    }
    SUBCASE("non-id types degrade to empty string — no UB / no throw") {
        CHECK(ComponentJsonIdToString(json(nullptr)) == "");
        CHECK(ComponentJsonIdToString(json(true)) == "");
        CHECK(ComponentJsonIdToString(json(3.14)) == "");
        CHECK(ComponentJsonIdToString(json::array()) == "");
        CHECK(ComponentJsonIdToString(json::object()) == "");
    }
}

TEST_CASE("ResolveComponentJsonBean unwraps componentBean / flat / null shapes") {
    SUBCASE("flat object returns &node") {
        json flat = {{"id", "10"}, {"name", "Engine"}};
        const json* p = ResolveComponentJsonBean(flat);
        REQUIRE(p != nullptr);
        CHECK(p == &flat);
    }
    SUBCASE("wrapped object returns the inner bean") {
        json wrapped = {{"componentBean", {{"id", "20"}, {"name", "Render"}}}};
        const json* p = ResolveComponentJsonBean(wrapped);
        REQUIRE(p != nullptr);
        CHECK(p != &wrapped);
        CHECK((*p)["id"] == "20");
        CHECK((*p)["name"] == "Render");
    }
    SUBCASE("componentBean must be an object to unwrap") {
        json wrappedNonObj = {{"componentBean", "stringy"}, {"id", "30"}, {"name", "Audio"}};
        const json* p = ResolveComponentJsonBean(wrappedNonObj);
        REQUIRE(p != nullptr);
        CHECK(p == &wrappedNonObj); // fallback to flat
    }
    SUBCASE("non-object inputs return nullptr — never UB") {
        CHECK(ResolveComponentJsonBean(json(nullptr)) == nullptr);
        CHECK(ResolveComponentJsonBean(json::array()) == nullptr);
        CHECK(ResolveComponentJsonBean(json("scalar")) == nullptr);
        CHECK(ResolveComponentJsonBean(json(42)) == nullptr);
    }
}

TEST_CASE("ExtractComponentOption parses required fields and skips malformed input") {
    SUBCASE("flat object with id+name succeeds and copies description+self into payload") {
        json node = {{"id", "10100"},
                     {"name", "Core"},
                     {"description", "Core systems"},
                     {"self", "https://example.atlassian.net/rest/api/3/component/10100"}};
        TrackerComponent c;
        TrackerFieldOption o;
        REQUIRE(ExtractComponentOption(node, c, o));
        CHECK(c.Id == "10100");
        CHECK(c.Name == "Core");
        CHECK(o.Id == "10100");
        CHECK(o.Value == "Core");
        CHECK(o.SecondaryValue == "Core systems");
        json payload = json::parse(o.PayloadJson, nullptr, false);
        REQUIRE_FALSE(payload.is_discarded());
        CHECK(payload["id"] == "10100");
        CHECK(payload["name"] == "Core");
        CHECK(payload["description"] == "Core systems");
        CHECK(payload["self"] == "https://example.atlassian.net/rest/api/3/component/10100");
    }
    SUBCASE("componentBean wrapping is unwrapped transparently") {
        json node = {{"componentBean", {{"id", 42}, {"name", "Wrapped"}}}};
        TrackerComponent c;
        TrackerFieldOption o;
        REQUIRE(ExtractComponentOption(node, c, o));
        CHECK(c.Id == "42");
        CHECK(c.Name == "Wrapped");
        CHECK(o.PayloadJson.find("\"id\":\"42\"") != std::string::npos);
    }
    SUBCASE("missing id rejects without mutating outputs") {
        json node = {{"name", "NoId"}};
        TrackerComponent c;
        TrackerFieldOption o;
        c.Id = "preserved-c";
        o.Id = "preserved-o";
        CHECK_FALSE(ExtractComponentOption(node, c, o));
        CHECK(c.Id == "preserved-c");
        CHECK(o.Id == "preserved-o");
    }
    SUBCASE("missing name rejects") {
        json node = {{"id", "1"}};
        TrackerComponent c;
        TrackerFieldOption o;
        CHECK_FALSE(ExtractComponentOption(node, c, o));
    }
    SUBCASE("non-string name rejects (even if id is valid)") {
        json node = {{"id", "1"}, {"name", 42}};
        TrackerComponent c;
        TrackerFieldOption o;
        CHECK_FALSE(ExtractComponentOption(node, c, o));
    }
    SUBCASE("empty id rejects") {
        json node = {{"id", ""}, {"name", "Has-Name"}};
        TrackerComponent c;
        TrackerFieldOption o;
        CHECK_FALSE(ExtractComponentOption(node, c, o));
    }
    SUBCASE("empty name rejects") {
        json node = {{"id", "1"}, {"name", ""}};
        TrackerComponent c;
        TrackerFieldOption o;
        CHECK_FALSE(ExtractComponentOption(node, c, o));
    }
    SUBCASE("non-object input rejects without UB") {
        TrackerComponent c;
        TrackerFieldOption o;
        CHECK_FALSE(ExtractComponentOption(json(nullptr), c, o));
        CHECK_FALSE(ExtractComponentOption(json::array(), c, o));
        CHECK_FALSE(ExtractComponentOption(json("scalar"), c, o));
    }
}

TEST_CASE("MergeComponentIntoCatalog dedupes, fills missing names, and routes options to the components field") {
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    fields.push_back(MakeComponentsField());

    TrackerComponent c1;
    c1.Id = "10";
    c1.Name = "Render";
    TrackerFieldOption o1;
    o1.Id = "10";
    o1.Value = "Render";

    SUBCASE("first merge appends new component + option") {
        MergeComponentIntoCatalog(fields, components, c1, o1);
        REQUIRE(components.size() == 1);
        CHECK(components[0].Id == "10");
        CHECK(components[0].Name == "Render");
        REQUIRE(fields[0].AllowedValueOptions.size() == 1);
        CHECK(fields[0].AllowedValueOptions[0].Id == "10");
    }

    SUBCASE("idempotent on same id — no duplicate component or option") {
        MergeComponentIntoCatalog(fields, components, c1, o1);
        MergeComponentIntoCatalog(fields, components, c1, o1);
        CHECK(components.size() == 1);
        CHECK(fields[0].AllowedValueOptions.size() == 1);
    }

    SUBCASE("existing component with blank name gets backfilled") {
        TrackerComponent placeholder;
        placeholder.Id = "10";
        placeholder.Name = "";
        components.push_back(placeholder);
        MergeComponentIntoCatalog(fields, components, c1, o1);
        REQUIRE(components.size() == 1);
        CHECK(components[0].Name == "Render");
    }

    SUBCASE("absent components field is tolerated — no crash, only catalog grows") {
        std::vector<TrackerField> emptyFields;
        MergeComponentIntoCatalog(emptyFields, components, c1, o1);
        CHECK(components.size() == 1);
        CHECK(emptyFields.empty());
    }

    SUBCASE("distinct ids accumulate") {
        TrackerComponent c2;
        c2.Id = "20";
        c2.Name = "Physics";
        TrackerFieldOption o2;
        o2.Id = "20";
        o2.Value = "Physics";
        MergeComponentIntoCatalog(fields, components, c1, o1);
        MergeComponentIntoCatalog(fields, components, c2, o2);
        CHECK(components.size() == 2);
        CHECK(fields[0].AllowedValueOptions.size() == 2);
        CHECK(FindOption(fields[0].AllowedValueOptions, "10") != nullptr);
        CHECK(FindOption(fields[0].AllowedValueOptions, "20") != nullptr);
    }
}

TEST_CASE("SortComponentCatalog sorts by lowercased name then id, syncs AllowedValues") {
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    fields.push_back(MakeComponentsField());

    auto add = [&](const std::string& id, const std::string& name) {
        TrackerComponent c;
        c.Id = id;
        c.Name = name;
        TrackerFieldOption o;
        o.Id = id;
        o.Value = name;
        MergeComponentIntoCatalog(fields, components, c, o);
    };

    SUBCASE("alphabetical, case-insensitive") {
        add("3", "zeta");
        add("1", "Alpha");
        add("2", "beta");
        SortComponentCatalog(fields, components);
        REQUIRE(components.size() == 3);
        CHECK(components[0].Name == "Alpha");
        CHECK(components[1].Name == "beta");
        CHECK(components[2].Name == "zeta");
        REQUIRE(fields[0].AllowedValueOptions.size() == 3);
        CHECK(fields[0].AllowedValueOptions[0].Value == "Alpha");
        CHECK(fields[0].AllowedValueOptions[2].Value == "zeta");
        // AllowedValues array tracks options after refresh.
        REQUIRE(fields[0].AllowedValues.size() == 3);
        CHECK(fields[0].AllowedValues[0] == "Alpha");
    }

    SUBCASE("collision on lowercased name breaks by Id ascending") {
        add("20", "Alpha");
        add("10", "alpha"); // same lowercase; smaller id first
        SortComponentCatalog(fields, components);
        REQUIRE(components.size() == 2);
        CHECK(components[0].Id == "10");
        CHECK(components[1].Id == "20");
    }

    SUBCASE("empty inputs do not crash and produce empty outputs") {
        std::vector<TrackerField> emptyFields;
        std::vector<TrackerComponent> emptyComps;
        SortComponentCatalog(emptyFields, emptyComps);
        CHECK(emptyFields.empty());
        CHECK(emptyComps.empty());
    }

    SUBCASE("absent components field — components still sorted") {
        std::vector<TrackerField> noCompFields;
        add("3", "gamma");
        add("1", "alpha");
        SortComponentCatalog(noCompFields, components);
        REQUIRE(components.size() == 2);
        CHECK(components[0].Name == "alpha");
        CHECK(components[1].Name == "gamma");
    }
}

TEST_CASE("ExtractComponentOption → Merge → Sort end-to-end mimics catalog assembly") {
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    fields.push_back(MakeComponentsField());

    const json input = json::array({
        {{"id", 30}, {"name", "Render"}, {"description", "rendering pipeline"}},
        {{"componentBean", {{"id", "10"}, {"name", "Audio"}}}},
        {{"id", "20"}, {"name", "physics"}},
        {{"id", "10"}, {"name", "Audio"}}, // duplicate — should dedupe
        {{"name", "no-id"}},                // malformed — skipped
    });

    int merged = 0;
    for (const auto& node : input) {
        TrackerComponent c;
        TrackerFieldOption o;
        if (ExtractComponentOption(node, c, o)) {
            MergeComponentIntoCatalog(fields, components, c, o);
            ++merged;
        }
    }
    CHECK(merged == 4);                // 5 input nodes; 1 malformed
    CHECK(components.size() == 3);     // 4 merged; 1 was a duplicate

    SortComponentCatalog(fields, components);
    REQUIRE(components.size() == 3);
    CHECK(components[0].Name == "Audio");
    CHECK(components[1].Name == "physics");
    CHECK(components[2].Name == "Render");
    REQUIRE(fields[0].AllowedValueOptions.size() == 3);
    CHECK(fields[0].AllowedValueOptions[0].Id == "10");
    CHECK(fields[0].AllowedValueOptions[1].Id == "20");
    CHECK(fields[0].AllowedValueOptions[2].Id == "30");
}
