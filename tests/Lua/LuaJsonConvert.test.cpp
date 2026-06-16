// LuaJsonConvert.test.cpp — unit coverage for the JSON <-> Lua marshalling leaf
// (Source/Core/include/Json/LuaJsonConvert.h) extracted from the two Lua binding
// TUs per debt luajson-convert-leaf / DRY Pillar 5. The converter used to live at
// file scope inside AppController_LuaBindings*.cpp (Class-C, ImGui-coupled) and so
// was untestable; lifting it to a header-only inline leaf is what makes these
// asserts possible. Covers the type map, array-vs-object detection, the round
// trip, and the three safety guards (JsonToLua depth + node caps, LuaToJson
// depth cap).
//
// Builds only inside SmatchetLuaTests (SMATCHET_WITH_LUA_AUTOMATION=1, sol2 + the
// production Lua static lib on the link line) — see tests/Lua/CMakeLists.txt.

#include "Json/LuaJsonConvert.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace {

// A sol::state with the base library open (create_table + table indexing).
sol::state MakeState() {
    sol::state L;
    L.open_libraries(sol::lib::base);
    return L;
}

} // namespace

TEST_CASE("LuaJsonConvert · JsonToLua maps every scalar type") {
    sol::state L = MakeState();
    sol::state_view view(L);

    nlohmann::json j = {
        {"name", "smatchet"}, {"count", 3}, {"ratio", 1.5}, {"on", true}, {"off", false}, {"empty", nullptr},
    };

    sol::table t = JsonToLua(view, j);
    CHECK(t["name"].get<std::string>() == "smatchet");
    CHECK(t["count"].get<double>() == doctest::Approx(3.0)); // integers land as Lua doubles
    CHECK(t["ratio"].get<double>() == doctest::Approx(1.5));
    CHECK(t["on"].get<bool>() == true);
    CHECK(t["off"].get<bool>() == false);
    CHECK(t["empty"].get<sol::object>().get_type() == sol::type::lua_nil);
}

TEST_CASE("LuaJsonConvert · JsonToLua arrays are 1-based and nest") {
    sol::state L = MakeState();
    sol::state_view view(L);

    nlohmann::json j = {
        {"tags", {"a", "b", "c"}},
        {"nested", {{"x", 10}}},
    };

    sol::table t = JsonToLua(view, j);
    sol::table tags = t["tags"];
    CHECK(tags[1].get<std::string>() == "a"); // Lua arrays start at index 1
    CHECK(tags[2].get<std::string>() == "b");
    CHECK(tags[3].get<std::string>() == "c");
    CHECK(t["nested"]["x"].get<double>() == doctest::Approx(10.0));
}

TEST_CASE("LuaJsonConvert · LuaToJson detects array vs object") {
    sol::state L = MakeState();

    SUBCASE("contiguous 1..N integer keys -> array") {
        sol::object arr = L.script("return {10, 20, 30}");
        nlohmann::json j = LuaToJson(arr);
        REQUIRE(j.is_array());
        REQUIRE(j.size() == 3);
        CHECK(j[0].get<double>() == doctest::Approx(10.0));
        CHECK(j[2].get<double>() == doctest::Approx(30.0));
    }

    SUBCASE("string keys -> object") {
        sol::object obj = L.script("return {alpha = 1, beta = 2}");
        nlohmann::json j = LuaToJson(obj);
        REQUIRE(j.is_object());
        CHECK(j["alpha"].get<double>() == doctest::Approx(1.0));
        CHECK(j["beta"].get<double>() == doctest::Approx(2.0));
    }

    SUBCASE("empty table -> object (max_idx == 0 branch)") {
        sol::object empty = L.script("return {}");
        nlohmann::json j = LuaToJson(empty);
        CHECK(j.is_object());
        CHECK(j.empty());
    }
}

TEST_CASE("LuaJsonConvert · round trip preserves structure") {
    sol::state L = MakeState();
    sol::state_view view(L);

    nlohmann::json original = {
        {"title", "round"},
        {"items", {1, 2, 3}},
        {"meta", {{"ok", true}}},
    };

    sol::object lua = JsonToLua(view, original);
    nlohmann::json back = LuaToJson(lua);

    REQUIRE(back.is_object());
    CHECK(back["title"].get<std::string>() == "round");
    REQUIRE(back["items"].is_array());
    CHECK(back["items"].size() == 3);
    CHECK(back["items"][1].get<double>() == doctest::Approx(2.0));
    CHECK(back["meta"]["ok"].get<bool>() == true);
}

TEST_CASE("LuaJsonConvert · JsonToLua depth cap returns nil past kJsonToLuaMaxDepth") {
    sol::state L = MakeState();
    sol::state_view view(L);

    nlohmann::json j = {{"x", 1}};
    std::size_t nodes = smatchet::lua_json_detail::kJsonToLuaMaxNodes;
    // depth one past the cap short-circuits before consuming a node.
    sol::object o =
        smatchet::lua_json_detail::JsonToLuaImpl(view, j, smatchet::lua_json_detail::kJsonToLuaMaxDepth + 1, nodes);
    CHECK(o.get_type() == sol::type::lua_nil);
    CHECK(nodes == smatchet::lua_json_detail::kJsonToLuaMaxNodes); // untouched
}

TEST_CASE("LuaJsonConvert · JsonToLua node budget stops conversion on overflow") {
    sol::state L = MakeState();
    sol::state_view view(L);

    nlohmann::json arr = {1, 2, 3};
    // Budget of 1 is spent on the root array; every element sees nodes == 0 and
    // converts to nil (graceful degradation, no throw).
    std::size_t nodes = 1;
    sol::table t = smatchet::lua_json_detail::JsonToLuaImpl(view, arr, 0, nodes);
    CHECK(t[1].get<sol::object>().get_type() == sol::type::lua_nil);
    CHECK(t[2].get<sol::object>().get_type() == sol::type::lua_nil);
    CHECK(nodes == 0);
}

TEST_CASE("LuaJsonConvert · LuaToJson depth cap returns null past 64") {
    sol::state L = MakeState();

    sol::object tbl = L.script("return {a = 1}");
    nlohmann::json j = smatchet::lua_json_detail::LuaToJsonImpl(tbl, 65);
    CHECK(j.is_null());
}
