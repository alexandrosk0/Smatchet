// BoundedJsonParse.test.cpp — pure doctest for the shared depth/node-bounded JSON
// parse helper (smatchet::json_safe::ParseBounded). This is the ONE bounded-parse
// implementation that the decode_json Lua sink, the MCP REST / JSON-RPC POST
// handlers, and the Lua-MCP-tool params path all route through (PR #1271). Testing
// the helper directly proves every ingress is depth/node bounded without standing
// up each transport.
//
// Pillar 3 — Never crash: a deeply-nested payload would stack-overflow a bare
// nlohmann::json::parse BEFORE any field is read. ParseBounded must reject it
// gracefully (null + non-empty errOut, NO throw, NO crash) and leave valid shallow
// JSON byte-identical.

#include <doctest/doctest.h>

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <string>

namespace js = smatchet::json_safe;

TEST_CASE("ParseBounded · valid shallow JSON parses unchanged" * doctest::test_suite("[high-risk]")) {
    std::string err;
    const nlohmann::json j = js::ParseBounded(R"({"a":1,"b":"two","c":[1,2,3],"d":{"e":true}})", err);
    REQUIRE(err.empty());
    REQUIRE(j.is_object());
    CHECK(j["a"].get<int>() == 1);
    CHECK(j["b"].get<std::string>() == "two");
    CHECK(j["c"].size() == 3);
    CHECK(j["c"][2].get<int>() == 3);
    CHECK(j["d"]["e"].get<bool>() == true);
}

TEST_CASE("ParseBounded · primitives and empty containers parse unchanged") {
    std::string err;
    CHECK(js::ParseBounded("42", err).get<int>() == 42);
    CHECK(err.empty());
    CHECK(js::ParseBounded(R"("hi")", err).get<std::string>() == "hi");
    CHECK(err.empty());
    CHECK(js::ParseBounded("true", err).get<bool>() == true);
    CHECK(err.empty());
    CHECK(js::ParseBounded("[]", err).is_array());
    CHECK(err.empty());
    CHECK(js::ParseBounded("{}", err).is_object());
    CHECK(err.empty());
}

TEST_CASE("ParseBounded · rejects over-deep nesting without crashing" * doctest::test_suite("[high-risk]")) {
    // 5000 '[' then 5000 ']' — depth 5000, well past the 256 cap. A bare
    // json::parse stack-overflows here (the remote-crash the fix closes); the
    // bounded helper must return null + the overflow error, no throw, no crash.
    const int n = 5000;
    std::string s(static_cast<std::size_t>(n), '[');
    s.append(static_cast<std::size_t>(n), ']');

    std::string err;
    const nlohmann::json j = js::ParseBounded(s, err);
    CHECK(j.is_null());
    REQUIRE_FALSE(err.empty());
    CHECK(err == std::string(js::OverflowError()));

    // The cap did not poison the parser — shallow JSON still parses after.
    std::string err2;
    const nlohmann::json ok = js::ParseBounded(R"({"a":[1,2,3]})", err2);
    REQUIRE(err2.empty());
    CHECK(ok["a"][0].get<int>() == 1);
}

TEST_CASE("ParseBounded · rejects exactly past the depth cap, accepts at the cap" * doctest::test_suite("[high-risk]")) {
    // A custom maxDepth makes the boundary cheap to assert. At depth == cap the
    // outermost container is admitted; one level deeper trips Descend().
    const int cap = 8;
    std::string atCap(static_cast<std::size_t>(cap), '[');
    atCap.append(static_cast<std::size_t>(cap), ']');
    std::string overCap(static_cast<std::size_t>(cap + 1), '[');
    overCap.append(static_cast<std::size_t>(cap + 1), ']');

    std::string err;
    CHECK(js::ParseBounded(atCap, err, /*maxBytes=*/1u << 20, /*maxDepth=*/cap).is_array());
    CHECK(err.empty());

    const nlohmann::json over = js::ParseBounded(overCap, err, /*maxBytes=*/1u << 20, /*maxDepth=*/cap);
    CHECK(over.is_null());
    CHECK(err == std::string(js::OverflowError()));
}

TEST_CASE("ParseBounded · rejects excessive node count" * doctest::test_suite("[high-risk]")) {
    // A flat array of 250000 zeros (> the 200000 node cap). Wide-but-shallow:
    // depth alone does not bound total allocation, so the node cap must catch it.
    std::string s = "[";
    for (int i = 0; i < 249999; ++i) {
        s += "0,";
    }
    s += "0]";

    std::string err;
    const nlohmann::json j = js::ParseBounded(s, err);
    CHECK(j.is_null());
    CHECK(err == std::string(js::OverflowError()));
}

TEST_CASE("ParseBounded · rejects oversized input by byte cap") {
    std::string err;
    const std::string s(2048, 'x'); // not even valid JSON — byte cap fires first
    const nlohmann::json j = js::ParseBounded(s, err, /*maxBytes=*/1024u);
    CHECK(j.is_null());
    CHECK(err == std::string(js::TooLargeError()));
}

TEST_CASE("ParseBounded · malformed-but-shallow input is InvalidJsonError, not overflow") {
    std::string err;
    const nlohmann::json j = js::ParseBounded("{not valid json", err);
    CHECK(j.is_null());
    CHECK(err == std::string(js::InvalidJsonError()));
}
