#include <doctest/doctest.h>

#include "JsonParseUtil.h"

#include <nlohmann/json.hpp>

#include <limits>
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
    std::string doubleEncoded = R"xx("{\"a\":2}")xx";
    CHECK(TryParseJsonMaybeDoubleEncoded(doubleEncoded, out));
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

// Regression (fuzz_jira_field_catalog): a Jira board/sprint page whose `id` is a
// float far outside int range — e.g. 2.4e56 — reached static_cast<int>(double),
// which is undefined behaviour (UBSan: "outside the range of representable values").
TEST_CASE("ParseJsonIntLoose takes the fallback for a float outside int range") {
    CHECK(ParseJsonIntLoose(json(2.44444e56), -1) == -1);
    CHECK(ParseJsonIntLoose(json(-2.44444e56), -1) == -1);
    CHECK(ParseJsonIntLoose(json(2147483648.0), -1) == -1);
    CHECK(ParseJsonIntLoose(json(-2147483649.0), -1) == -1);
    // The extremes that still truncate into range convert as before.
    CHECK(ParseJsonIntLoose(json(2147483647.9), -1) == (std::numeric_limits<int>::max)());
    CHECK(ParseJsonIntLoose(json(-2147483648.9), -1) == (std::numeric_limits<int>::min)());
    CHECK(ParseJsonIntLoose(json(-0.5), -1) == 0);
}

TEST_CASE("ParseJsonIntLoose takes the fallback for an integer outside int range instead of wrapping") {
    // 4294967300 wrapped to 4 — a different board id — before the range check.
    CHECK(ParseJsonIntLoose(json(4294967300LL), -1) == -1);
    CHECK(ParseJsonIntLoose(json(4294967300ULL), -1) == -1);
    CHECK(ParseJsonIntLoose(json(-2147483649LL), -1) == -1);
    CHECK(ParseJsonIntLoose(json(18446744073709551615ULL), -1) == -1);
    CHECK(ParseJsonIntLoose(json(2147483647ULL), -1) == (std::numeric_limits<int>::max)());
    CHECK(ParseJsonIntLoose(json(-2147483648LL), -1) == (std::numeric_limits<int>::min)());
}

TEST_CASE("ParseJsonInt64Loose takes the fallback for an unsigned value above long long range") {
    CHECK(ParseJsonInt64Loose(json(18446744073709551615ULL), -1LL) == -1LL);
    CHECK(ParseJsonInt64Loose(json(9223372036854775808ULL), -1LL) == -1LL);
    CHECK(ParseJsonInt64Loose(json(9223372036854775807ULL), -1LL) == (std::numeric_limits<long long>::max)());
}

TEST_CASE("ParseJsonInt64Loose takes the fallback for a float outside long long range") {
    CHECK(ParseJsonInt64Loose(json(2.44444e56), -1LL) == -1LL);
    CHECK(ParseJsonInt64Loose(json(-1e19), -1LL) == -1LL);
    CHECK(ParseJsonInt64Loose(json(9223372036854775808.0), -1LL) == -1LL);
    CHECK(ParseJsonInt64Loose(json(-9223372036854775808.0), -1LL) == (std::numeric_limits<long long>::min)());
    CHECK(ParseJsonInt64Loose(json(4096.75), -1LL) == 4096LL);
}
