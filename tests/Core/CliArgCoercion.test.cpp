// CoerceCliArgValue — typed-arg coercion for the unified CLI's `--key=value`
// parser. See Source/Standalone/CliArgCoercion.h for the contract.
//
// Originating audit: perf-detective P2 tooling finding on develop@31e1893
// ("scenario.run CLI cannot pass typed scalar args"). Without this helper the
// CLI passes every value as a JSON string and handlers asking for bool/int via
// `args.value("flag", false)` raise json.exception.type_error.302.

#include <doctest/doctest.h>

#include "CliArgCoercion.h"

#include <nlohmann/json.hpp>

using nlohmann::json;
using smatchet::cli::CoerceCliArgValue;

TEST_CASE("CoerceCliArgValue maps true / false to JSON boolean") {
    const json t = CoerceCliArgValue("true");
    REQUIRE(t.is_boolean());
    CHECK(t.get<bool>() == true);

    const json f = CoerceCliArgValue("false");
    REQUIRE(f.is_boolean());
    CHECK(f.get<bool>() == false);
}

TEST_CASE("CoerceCliArgValue is case-sensitive on bool keywords") {
    // Capitalised / mixed-case stays a string — handlers can opt into looser
    // semantics in their own SafeBool reader, but the wire format is strict.
    CHECK(CoerceCliArgValue("True").is_string());
    CHECK(CoerceCliArgValue("TRUE").is_string());
    CHECK(CoerceCliArgValue("False").is_string());
    CHECK(CoerceCliArgValue("FALSE").is_string());
}

TEST_CASE("CoerceCliArgValue maps strict integers to JSON integer") {
    const json zero = CoerceCliArgValue("0");
    REQUIRE(zero.is_number_integer());
    CHECK(zero.get<long long>() == 0);

    const json positive = CoerceCliArgValue("42");
    REQUIRE(positive.is_number_integer());
    CHECK(positive.get<long long>() == 42);

    const json negative = CoerceCliArgValue("-7");
    REQUIRE(negative.is_number_integer());
    CHECK(negative.get<long long>() == -7);

    const json large = CoerceCliArgValue("9223372036854775807"); // INT64_MAX
    REQUIRE(large.is_number_integer());
    CHECK(large.get<long long>() == 9223372036854775807LL);
}

TEST_CASE("CoerceCliArgValue maps strict decimals to JSON number") {
    const json simple = CoerceCliArgValue("1.5");
    REQUIRE(simple.is_number_float());
    CHECK(simple.get<double>() == doctest::Approx(1.5));

    const json neg = CoerceCliArgValue("-0.25");
    REQUIRE(neg.is_number_float());
    CHECK(neg.get<double>() == doctest::Approx(-0.25));

    const json sci = CoerceCliArgValue("1.5e3");
    REQUIRE(sci.is_number_float());
    CHECK(sci.get<double>() == doctest::Approx(1500.0));

    const json negSci = CoerceCliArgValue("-2.5E-2");
    REQUIRE(negSci.is_number_float());
    CHECK(negSci.get<double>() == doctest::Approx(-0.025));
}

TEST_CASE("CoerceCliArgValue keeps non-numeric / non-bool tokens as strings") {
    // Literal text — preserved as-is.
    CHECK(CoerceCliArgValue("hello").is_string());
    CHECK(CoerceCliArgValue("hello").get<std::string>() == "hello");

    // Empty string.
    CHECK(CoerceCliArgValue("").is_string());

    // Mixed alphanumerics — must NOT coerce.
    CHECK(CoerceCliArgValue("42x").is_string());
    CHECK(CoerceCliArgValue("x42").is_string());

    // Looks like a float but missing digits — must stay a string.
    CHECK(CoerceCliArgValue("1.").is_string());
    CHECK(CoerceCliArgValue(".5").is_string());
    CHECK(CoerceCliArgValue("-").is_string());

    // Exponent without digits.
    CHECK(CoerceCliArgValue("1.5e").is_string());
    CHECK(CoerceCliArgValue("1.5e+").is_string());

    // Hex / octal not in scope — strict decimal only.
    CHECK(CoerceCliArgValue("0x10").is_string());

    // Sentinels users might want literal — wrap in arg-json envelope if you
    // actually want them as JSON null / nan; the bare CLI form is a string.
    CHECK(CoerceCliArgValue("null").is_string());
    CHECK(CoerceCliArgValue("nan").is_string());

    // JSON-looking text — must stay string (envelope syntax deferred).
    CHECK(CoerceCliArgValue("[1,2,3]").is_string());
    CHECK(CoerceCliArgValue("{\"a\":1}").is_string());

    // Plain integer with trailing whitespace — not strict, stay string.
    CHECK(CoerceCliArgValue("42 ").is_string());
    CHECK(CoerceCliArgValue(" 42").is_string());
}

TEST_CASE("CoerceCliArgValue falls back to string for out-of-range integers") {
    // Way past INT64_MAX → std::stoll throws → preserve the literal as string
    // so the user's intent survives instead of overflowing silently.
    const json overflow = CoerceCliArgValue("99999999999999999999");
    CHECK(overflow.is_string());
    CHECK(overflow.get<std::string>() == "99999999999999999999");
}

TEST_CASE("IsValidMcpPort accepts 1..65535, rejects out-of-range") {
    using smatchet::cli::IsValidMcpPort;
    CHECK(IsValidMcpPort(1));
    CHECK(IsValidMcpPort(42360));
    CHECK(IsValidMcpPort(65535));
    CHECK_FALSE(IsValidMcpPort(0));
    CHECK_FALSE(IsValidMcpPort(-1));
    CHECK_FALSE(IsValidMcpPort(65536));
    CHECK_FALSE(IsValidMcpPort(99999));
}

TEST_CASE("ClampScenarioFrames is non-negative and overflow-safe") {
    using smatchet::cli::ClampScenarioFrames;
    CHECK(ClampScenarioFrames(600) == 600);
    CHECK(ClampScenarioFrames(0) == 0);
    CHECK(ClampScenarioFrames(-100) == 0);
    // A near-INT_MAX request would overflow the wait-ms arithmetic; the clamp
    // keeps the downstream computation inside int.
    const int clamped = ClampScenarioFrames(2147483647LL);
    CHECK(clamped > 0);
    const long long waitMs = (static_cast<long long>(clamped) / 60 + 30) * 1000;
    CHECK(waitMs <= 2147483647LL);
}

TEST_CASE("ScenarioFramesFromJson reads + clamps, falling back on bad input") {
    using smatchet::cli::ScenarioFramesFromJson;
    CHECK(ScenarioFramesFromJson(json(300), 600) == 300);
    CHECK(ScenarioFramesFromJson(json("450"), 600) == 450);
    CHECK(ScenarioFramesFromJson(json(-5), 600) == 0);
    CHECK(ScenarioFramesFromJson(json("not-a-number"), 600) == 600);
    CHECK(ScenarioFramesFromJson(json(true), 600) == 600);
    CHECK(ScenarioFramesFromJson(json(2147483647LL), 600) <= 2000000);
}

TEST_CASE("ScenarioWaitMs: explicit --timeout wins over the frames-derived budget") {
    using smatchet::cli::ScenarioWaitMs;
    // A deliberately small timeout must shorten the budget — the --spawn path
    // once ignored it entirely (issue #1943), so this is the divergence guard.
    CHECK(ScenarioWaitMs(500, 3600) == 500);
    // And a large one must lengthen it past the frames-derived value.
    CHECK(ScenarioWaitMs(600000, 600) == 600000);
}

TEST_CASE("ScenarioWaitMs: characterization of the frames-derived fallback") {
    using smatchet::cli::ScenarioWaitMs;
    // Anchor to the known-good in-process behaviour: (frames / 60 + 30) * 1000.
    CHECK(ScenarioWaitMs(0, 600) == 40000);   // default frames -> 40 s
    CHECK(ScenarioWaitMs(0, 3600) == 90000);  // the CI bucket-E derivation
    CHECK(ScenarioWaitMs(0, 0) == 30000);     // startup/teardown floor
    // Unset and nonsensical timeouts both fall back.
    CHECK(ScenarioWaitMs(-1, 600) == 40000);
}
