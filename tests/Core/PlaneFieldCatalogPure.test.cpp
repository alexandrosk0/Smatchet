// Monoliths Phase A of decompose-top-20-monoliths — pure-logic doctest
// for the Plane field-catalog property → TrackerField mapper. No HTTP, no PlaneClient.
// Goldens captured from the pre-refactor TrackerFieldFromPlaneProperty behaviour.

#include "PlaneFieldCatalogPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using nlohmann::json;
using smatchet::plane::AppendPlaneRowOption;
using smatchet::plane::MapPlanePropertyToField;

namespace {

json MakeProp(const std::string& id, const std::string& propertyType) {
    json p;
    p["id"] = id;
    p["property_type"] = propertyType;
    return p;
}

} // namespace

TEST_CASE("MapPlanePropertyToField — rejects nodes without a usable id") {
    json p;
    p["property_type"] = "TEXT";
    TrackerField out;
    CHECK_FALSE(MapPlanePropertyToField(p, out));
}

TEST_CASE("MapPlanePropertyToField — name falls back display_name → name → id") {
    {
        json p = MakeProp("f1", "TEXT");
        p["display_name"] = "Display";
        p["name"] = "Raw";
        TrackerField out;
        REQUIRE(MapPlanePropertyToField(p, out));
        CHECK(out.Name == "Display");
    }
    {
        json p = MakeProp("f2", "TEXT");
        p["name"] = "Raw";
        TrackerField out;
        REQUIRE(MapPlanePropertyToField(p, out));
        CHECK(out.Name == "Raw");
    }
    {
        json p = MakeProp("f3", "TEXT");
        TrackerField out;
        REQUIRE(MapPlanePropertyToField(p, out));
        CHECK(out.Name == "f3");
    }
}

TEST_CASE("MapPlanePropertyToField — property_type classification (case-insensitive)") {
    struct Case {
        const char* pt;
        const char* type;
        TrackerFieldFamily fam;
        bool isArray;
        bool isUser;
    };
    const Case cases[] = {
        {"number", "number", TrackerFieldFamily::Number, false, false},
        {"INTEGER", "number", TrackerFieldFamily::Number, false, false},
        {"DECIMAL", "number", TrackerFieldFamily::Number, false, false},
        {"FLOAT", "number", TrackerFieldFamily::Number, false, false},
        {"DATE", "date", TrackerFieldFamily::Date, false, false},
        {"DATETIME", "datetime", TrackerFieldFamily::DateTime, false, false},
        {"TIMESTAMP", "datetime", TrackerFieldFamily::DateTime, false, false},
        {"OPTION", "option", TrackerFieldFamily::SelectSingle, false, false},
        {"DROPDOWN", "option", TrackerFieldFamily::SelectSingle, false, false},
        {"SELECT", "option", TrackerFieldFamily::SelectSingle, false, false},
        {"MULTI_SELECT", "array", TrackerFieldFamily::SelectMulti, true, false},
        {"MULTISELECT", "array", TrackerFieldFamily::SelectMulti, true, false},
        {"MEMBER", "user", TrackerFieldFamily::UserSingle, false, true},
        {"USER", "user", TrackerFieldFamily::UserSingle, false, true},
        {"MULTI_MEMBER", "array", TrackerFieldFamily::UserMulti, true, true},
        {"MULTI_USER", "array", TrackerFieldFamily::UserMulti, true, true},
        {"USERS", "array", TrackerFieldFamily::UserMulti, true, true},
        {"BOOLEAN", "boolean", TrackerFieldFamily::Text, false, false},
        {"CHECKBOX", "boolean", TrackerFieldFamily::Text, false, false},
        {"RICH_TEXT", "string", TrackerFieldFamily::Text, false, false},
        {"URL", "string", TrackerFieldFamily::Text, false, false},
        {"", "string", TrackerFieldFamily::Text, false, false},
        {"SOMETHING_UNKNOWN", "string", TrackerFieldFamily::Text, false, false},
    };
    for (const auto& c : cases) {
        TrackerField out;
        REQUIRE(MapPlanePropertyToField(MakeProp("id", c.pt), out));
        CHECK(out.Type == c.type);
        CHECK(out.Family == c.fam);
        CHECK(out.IsArray == c.isArray);
        CHECK(out.IsUserType == c.isUser);
        CHECK(out.IsCustom);
    }
}

TEST_CASE("MapPlanePropertyToField — OPTION parses nested options (value falls back to name)") {
    json p = MakeProp("opt", "OPTION");
    p["options"] = json::array({
        {{"id", "o1"}, {"value", "One"}},
        {{"id", "o2"}, {"name", "Two"}}, // value missing → name
        {{"id", ""}, {"value", ""}},     // empty id+value → skipped
        json("not-an-object"),           // non-object → skipped
    });
    TrackerField out;
    REQUIRE(MapPlanePropertyToField(p, out));
    REQUIRE(out.AllowedValueOptions.size() == 2);
    CHECK(out.AllowedValueOptions[0].Id == "o1");
    CHECK(out.AllowedValueOptions[0].Value == "One");
    CHECK(out.AllowedValueOptions[1].Id == "o2");
    CHECK(out.AllowedValueOptions[1].Value == "Two");
}

TEST_CASE("MapPlanePropertyToField — readonly + required flags + raw json dump") {
    json p = MakeProp("f", "TEXT");
    p["is_readonly"] = true;
    p["is_required"] = true;
    TrackerField out;
    REQUIRE(MapPlanePropertyToField(p, out));
    CHECK(out.ReadOnly);
    CHECK(out.IsRequired);
    CHECK_FALSE(out.RawFieldDefinitionJson.empty());
}

TEST_CASE("AppendPlaneRowOption — id required; value falls back from name to id") {
    {
        TrackerField field;
        json row;
        row["id"] = "S1";
        row["name"] = "Todo";
        const std::string id = AppendPlaneRowOption(row, "id", "name", field);
        CHECK(id == "S1");
        REQUIRE(field.AllowedValueOptions.size() == 1);
        CHECK(field.AllowedValueOptions[0].Id == "S1");
        CHECK(field.AllowedValueOptions[0].Value == "Todo");
    }
    {
        TrackerField field;
        json row;
        row["id"] = "S2"; // name missing → value falls back to id
        const std::string id = AppendPlaneRowOption(row, "id", "name", field);
        CHECK(id == "S2");
        REQUIRE(field.AllowedValueOptions.size() == 1);
        CHECK(field.AllowedValueOptions[0].Value == "S2");
    }
    {
        TrackerField field;
        json row;
        row["name"] = "no-id"; // id missing → skipped
        const std::string id = AppendPlaneRowOption(row, "id", "name", field);
        CHECK(id.empty());
        CHECK(field.AllowedValueOptions.empty());
    }
}
