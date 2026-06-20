// ParamBoundsPure — bucket-A coverage for the opt-in numeric-range + max-length
// argument bounds (command-input-hardening Phase 0). Pure header-only check
// (Source/Core/include/Commands/ParamBoundsPure.h); no registry link, mirroring
// CommandSourceTrust.test.cpp. Closes the unbounded-Int (frames overflow) +
// unbounded-String argument vectors at the shared command chokepoint.

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "Commands/ParamBoundsPure.h"

using smatchet::cmd::ParamSpec;
using smatchet::cmd::ParamType;
using smatchet::cmd::ParamValueWithinBounds;

namespace {
ParamSpec IntParam(long long lo, long long hi) {
    ParamSpec p;
    p.Name = "limit";
    p.Type = ParamType::Int;
    p.MinInt = std::make_shared<long long>(lo);
    p.MaxInt = std::make_shared<long long>(hi);
    return p;
}
} // namespace

TEST_CASE("Int within [0,500] passes; over/under fail with structured detail") {
    const ParamSpec p = IntParam(0, 500);
    std::string why;
    nlohmann::json detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json(50), why, detail) == true);

    why.clear();
    detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json(999999), why, detail) == false);
    CHECK(detail.contains("max"));

    why.clear();
    detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json(-1), why, detail) == false);
    CHECK(detail.contains("min"));
}

TEST_CASE("Unset bounds are unbounded (opt-in per ParamSpec)") {
    ParamSpec p;
    p.Name = "n";
    p.Type = ParamType::Int; // no MinInt/MaxInt set
    std::string why;
    nlohmann::json detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json(2147483647), why, detail) == true);
}

TEST_CASE("MaxLen caps string length in bytes") {
    ParamSpec p;
    p.Name = "s";
    p.Type = ParamType::String;
    p.MaxLen = 8;
    std::string why;
    nlohmann::json detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json("short"), why, detail) == true);

    why.clear();
    detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json(std::string(100, 'x')), why, detail) == false);
    CHECK(detail.contains("maxLen"));
}

TEST_CASE("Number param honors MinInt/MaxInt as a floating bound") {
    ParamSpec p;
    p.Name = "ratio";
    p.Type = ParamType::Number;
    p.MinInt = std::make_shared<long long>(0);
    p.MaxInt = std::make_shared<long long>(10);
    std::string why;
    nlohmann::json detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json(3.5), why, detail) == true);

    why.clear();
    detail = nlohmann::json::object();
    CHECK(ParamValueWithinBounds(p, nlohmann::json(10.5), why, detail) == false);
    CHECK(detail.contains("max"));
}
