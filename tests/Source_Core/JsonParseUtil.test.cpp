#include <doctest/doctest.h>

#include "JsonParseUtil.h"

#include <nlohmann/json.hpp>

#include <string>

using nlohmann::json;

TEST_CASE("TryParseJsonMaybeDoubleEncoded handles plain JSON") {
    json out;
    CHECK(TryParseJsonMaybeDoubleEncoded(R"({"a":1})", out));
    REQUIRE(out.is_object());
    CHECK(out["a"].get<int>() == 1);
}

TEST_CASE("TryParseJsonMaybeDoubleEncoded unwraps double-encoded JSON") {
    json out;
    CHECK(TryParseJsonMaybeDoubleEncoded(R"("{\"a\":2}")", out));
    REQUIRE(out.is_object());
    CHECK(out["a"].get<int>() == 2);
}

TEST_CASE("TryParseJsonMaybeDoubleEncoded returns false on malformed input") {
    json out;
    CHECK_FALSE(TryParseJsonMaybeDoubleEncoded("{not json", out));
    CHECK(out.is_null());
}

TEST_CASE("TryParseJsonMaybeDoubleEncoded returns false when inner string is not JSON") {
    json out;
    CHECK_FALSE(TryParseJsonMaybeDoubleEncoded(R"("not json either")", out));
    CHECK(out.is_null());
}

TEST_CASE("ParseJsonIntLoose accepts every numeric encoding") {
    CHECK(ParseJsonIntLoose(json(42)) == 42);
    CHECK(ParseJsonIntLoose(json(42u)) == 42);
    CHECK(ParseJsonIntLoose(json(42.7)) == 42);
    CHECK(ParseJsonIntLoose(json("42")) == 42);
}

TEST_CASE("ParseJsonIntLoose falls back on unparseable strings + non-numeric types") {
    CHECK(ParseJsonIntLoose(json("not a number"), -1) == -1);
    CHECK(ParseJsonIntLoose(json(nullptr), 7) == 7);
    CHECK(ParseJsonIntLoose(json(json::value_t::null), 9) == 9);
    CHECK(ParseJsonIntLoose(json::array({1, 2}), 5) == 5);
    CHECK(ParseJsonIntLoose(json::object({{"k", 1}}), 5) == 5);
}

TEST_CASE("ParseJsonInt64Loose preserves 64-bit values") {
    const long long big = 9000000000LL;
    CHECK(ParseJsonInt64Loose(json(big)) == big);
    CHECK(ParseJsonInt64Loose(json(std::to_string(big))) == big);
    CHECK(ParseJsonInt64Loose(json("garbage"), 123LL) == 123LL);
}

TEST_CASE("ParseJsonIntFieldLoose returns fallback when key is missing") {
    const json obj = json::object({{"present", 7}});
    CHECK(ParseJsonIntFieldLoose(obj, "present", 0) == 7);
    CHECK(ParseJsonIntFieldLoose(obj, "absent", -1) == -1);
}

TEST_CASE("ParseJsonInt64FieldLoose handles present + missing keys") {
    const json obj = json::object({{"big", 9000000000LL}});
    CHECK(ParseJsonInt64FieldLoose(obj, "big", 0) == 9000000000LL);
    CHECK(ParseJsonInt64FieldLoose(obj, "missing", 42LL) == 42LL);
}
