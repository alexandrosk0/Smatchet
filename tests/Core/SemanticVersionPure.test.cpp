// Unit tests for smatchet::version::ParseSemanticVersion / CompareSemanticVersion
// (SemanticVersionPure.cpp) — the update-check release-tag parser extracted from
// AttachmentAppUpdateService so it can be tested + fuzzed without cpr. Pins the parse contract
// (v-prefix + prerelease stripping, all-digit segment gate, INT_MAX range gate — CPP_CODE_AUDIT.md
// #17) and the (Major, Minor, Patch) ordering. The fuzz target (tests/fuzz/fuzz_semver_parse.cpp)
// covers the no-crash/no-hang/no-overflow property over arbitrary bytes; these pin exact values.
#include "SemanticVersionPure.h"

#include <doctest/doctest.h>

using smatchet::version::CompareSemanticVersion;
using smatchet::version::ParseSemanticVersion;
using smatchet::version::SemanticVersion;

TEST_CASE("ParseSemanticVersion: well-formed tags parse to their components") {
    SUBCASE("plain triple") {
        const SemanticVersion v = ParseSemanticVersion("1.2.3");
        CHECK(v.Valid);
        CHECK(v.Major == 1);
        CHECK(v.Minor == 2);
        CHECK(v.Patch == 3);
    }
    SUBCASE("leading v is stripped") {
        const SemanticVersion v = ParseSemanticVersion("v10.20.30");
        CHECK(v.Valid);
        CHECK(v.Major == 10);
        CHECK(v.Minor == 20);
        CHECK(v.Patch == 30);
    }
    SUBCASE("a -prerelease suffix is dropped") {
        const SemanticVersion v = ParseSemanticVersion("v2.3.4-rc1");
        CHECK(v.Valid);
        CHECK(v.Major == 2);
        CHECK(v.Minor == 3);
        CHECK(v.Patch == 4);
    }
}

TEST_CASE("ParseSemanticVersion: malformed tags are rejected without throwing or overflowing") {
    SUBCASE("too few segments") { CHECK_FALSE(ParseSemanticVersion("1.2").Valid); }
    SUBCASE("too many segments") { CHECK_FALSE(ParseSemanticVersion("1.2.3.4").Valid); }
    SUBCASE("empty segment") { CHECK_FALSE(ParseSemanticVersion("1..3").Valid); }
    SUBCASE("non-digit segment") { CHECK_FALSE(ParseSemanticVersion("vabc.def.ghi").Valid); }
    SUBCASE("empty string") { CHECK_FALSE(ParseSemanticVersion("").Valid); }
    SUBCASE("digit run past INT_MAX is rejected, not UB (#17)") {
        // 99999999999999999999 > LLONG_MAX -> stoll throws -> caught -> invalid; and any value in
        // (INT_MAX, LLONG_MAX] is range-gated to invalid. Neither path overflows.
        CHECK_FALSE(ParseSemanticVersion("v99999999999999999999.0.0").Valid);
        CHECK_FALSE(ParseSemanticVersion("2147483648.0.0").Valid); // INT_MAX + 1
        CHECK(ParseSemanticVersion("2147483647.0.0").Valid);       // exactly INT_MAX is allowed
    }
}

TEST_CASE("CompareSemanticVersion orders by Major, then Minor, then Patch") {
    const SemanticVersion a = ParseSemanticVersion("1.2.3");
    CHECK(CompareSemanticVersion(a, ParseSemanticVersion("1.2.3")) == 0);
    CHECK(CompareSemanticVersion(a, ParseSemanticVersion("2.0.0")) < 0);
    CHECK(CompareSemanticVersion(a, ParseSemanticVersion("1.3.0")) < 0);
    CHECK(CompareSemanticVersion(a, ParseSemanticVersion("1.2.4")) < 0);
    CHECK(CompareSemanticVersion(a, ParseSemanticVersion("1.2.2")) > 0);
    CHECK(CompareSemanticVersion(a, ParseSemanticVersion("0.9.9")) > 0);
}
